#pragma once
// Base limit order book: flat price array + intrusive doubly linked lists
// + preallocated node pool with an intrusive free list.
//
// Order id lookup: a flat array indexed directly by order_id (ids must be
// in [0, max_order_id)). Assumes ids are dense, like exchange-assigned
// sequential order reference numbers.
//
// Matching: price-time priority. An incoming order trades against the best
// opposite level(s) while prices cross, oldest resting order first, at the
// resting order's price. Any remainder rests in the book (limit orders).
//
// Best bid/ask: cached level indices. When the best level empties we scan
// for the next non-empty one (O(levels) worst case - a bitmap could make
// this ~O(1) later if it shows up in measurements).
//
// Trade reporting: a callback invoked once per fill (template parameter,
// so it's inlined - no allocation, no function-pointer call).
//
// Self-trade prevention (STP): each order may carry an owner id. If an
// incoming order would match a resting order with the same owner, matching
// stops and the incoming order's remainder is cancelled ("cancel newest").
// Fills that already happened against other owners stand.
//
// Pool full: if there's no node to rest the remainder, it's cancelled and
// reported in AddResult (never silently dropped). Size the pool for peak load.
//
// Not done yet:
//   - market / IOC orders, order modify

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <optional>
#include <vector>

using Idx = uint32_t;                 // index into the node pool
inline constexpr Idx NIL = UINT32_MAX; // "no node"

enum class Side : uint8_t { Buy, Sell };

using Owner = uint32_t;                 // trader / account id, for self-trade prevention
inline constexpr Owner kNoOwner = 0;    // STP disabled for this order

// One resting order. Lives in the pool. `prev`/`next` link it into its
// price level's FIFO queue while in use, and `next` links it into the
// free list while unused.
struct Node {
    uint64_t order_id;
    uint32_t qty;
    uint32_t price_idx; // index into the ladder, so cancel can find its level
    Idx      prev;
    Idx      next;
    Owner    owner;
    Side     side;
};
static_assert(sizeof(Node) == 32, "Node should stay 2 per cache line");

// ---------------------------------------------------------------------------
// Node pool: all nodes allocated once up front. alloc/free are O(1) stack
// pop/push on a free list threaded through the free nodes' `next` fields.
// ---------------------------------------------------------------------------
class NodePool {
public:
    explicit NodePool(Idx capacity) : nodes_(capacity) {
        // Chain every node: 0 -> 1 -> 2 -> ... -> NIL
        for (Idx i = 0; i + 1 < capacity; ++i) nodes_[i].next = i + 1;
        if (capacity > 0) nodes_[capacity - 1].next = NIL;
        free_head_ = capacity > 0 ? 0 : NIL;
    }

    // Pop: returns NIL if the pool is exhausted.
    Idx alloc() {
        Idx i = free_head_;
        if (i == NIL) return NIL;
        free_head_ = nodes_[i].next;
        return i;
    }

    // Push: the caller must make sure `i` is currently in use (no double free).
    void free(Idx i) {
        nodes_[i].next = free_head_;
        free_head_ = i;
    }

    Node&       operator[](Idx i)       { return nodes_[i]; }
    const Node& operator[](Idx i) const { return nodes_[i]; }

private:
    std::vector<Node> nodes_;
    Idx free_head_;
};

// ---------------------------------------------------------------------------
// One price level: a FIFO queue (time priority) of pool nodes + aggregates.
// ---------------------------------------------------------------------------
struct Level {
    Idx      head      = NIL; // oldest order: matched first
    Idx      tail      = NIL; // newest order: new orders append here
    uint64_t total_qty = 0;
    uint32_t count     = 0;
};

// One execution. Reported to the caller's callback during add().
struct Trade {
    uint64_t taker_id;    // incoming (aggressive) order
    uint64_t maker_id;    // resting (passive) order
    int64_t  price_ticks; // always the resting order's price
    uint32_t qty;
    Side     taker_side;
};

enum class CancelReason : uint8_t {
    None,      // nothing was cancelled
    SelfTrade, // would have matched an order from the same owner
    PoolFull,  // no free node to rest the remainder
};

struct AddResult {
    bool         accepted  = false; // false: bad price/id, nothing happened
    uint32_t     filled    = 0;     // shares that traded immediately
    uint32_t     cancelled = 0;     // shares neither filled nor rested (see reason)
    CancelReason reason    = CancelReason::None;
    Idx          handle    = NIL;   // node of the resting remainder, NIL if none rested
};

// ---------------------------------------------------------------------------
// Order book. Prices are integer ticks (no floating point).
// Slot i in each ladder represents price (min_price_ticks + i).
// ---------------------------------------------------------------------------
class OrderBook {
public:
    // max_order_id: ids must be < this. Defaults to max_orders.
    OrderBook(int64_t min_price_ticks, uint32_t num_levels, Idx max_orders,
              uint64_t max_order_id = 0)
        : min_price_(min_price_ticks),
          num_levels_(num_levels),
          bids_(num_levels),
          asks_(num_levels),
          pool_(max_orders),
          id_to_node_(max_order_id ? max_order_id : max_orders, NIL),
          best_bid_(-1),
          best_ask_(num_levels) {}

    // Submit a limit order. Matches first, then rests any remainder.
    // `on_trade(const Trade&)` is called once per fill. It's a template
    // parameter, so the compiler inlines it (no function-pointer call).
    // The callback must not modify the book.
    //
    // Invariant: filled + cancelled + (resting qty) == qty.
    template <class OnTrade>
        requires std::invocable<OnTrade&, const Trade&>
    AddResult add(uint64_t order_id, Side side, int64_t price_ticks, uint32_t qty,
                  Owner owner, OnTrade&& on_trade) {
        AddResult res;
        int64_t offset = price_ticks - min_price_;
        if (offset < 0 || offset >= static_cast<int64_t>(num_levels_)) return res;
        if (order_id >= id_to_node_.size() || id_to_node_[order_id] != NIL) return res;
        if (qty == 0) return res;
        res.accepted = true;

        auto pi = static_cast<uint32_t>(offset);
        uint32_t remaining = qty;
        bool self_trade = false;

        // 1. Match against the opposite side while prices cross.
        if (side == Side::Buy) {
            while (remaining > 0 && !self_trade && best_ask_ <= static_cast<int64_t>(pi)) {
                remaining = match_level(asks_, static_cast<uint32_t>(best_ask_),
                                        order_id, side, owner, remaining, self_trade, on_trade);
                if (asks_[best_ask_].count == 0) best_ask_ = next_ask_from(best_ask_ + 1);
            }
        } else {
            while (remaining > 0 && !self_trade && best_bid_ >= static_cast<int64_t>(pi)) {
                remaining = match_level(bids_, static_cast<uint32_t>(best_bid_),
                                        order_id, side, owner, remaining, self_trade, on_trade);
                if (bids_[best_bid_].count == 0) best_bid_ = next_bid_from(best_bid_ - 1);
            }
        }
        res.filled = qty - remaining;
        if (remaining == 0) return res;

        if (self_trade) { // cancel newest: the incoming remainder never rests
            res.cancelled = remaining;
            res.reason = CancelReason::SelfTrade;
            return res;
        }

        // 2. Rest the remainder.
        Idx n = pool_.alloc();
        if (n == NIL) {
            res.cancelled = remaining;
            res.reason = CancelReason::PoolFull;
            return res;
        }
        id_to_node_[order_id] = n;
        pool_[n] = Node{order_id, remaining, pi, NIL, NIL, owner, side};

        Level& lvl = ladder(side)[pi];
        link_tail(lvl, n);
        lvl.total_qty += remaining;
        lvl.count += 1;

        if (side == Side::Buy) best_bid_ = std::max<int64_t>(best_bid_, pi);
        else                   best_ask_ = std::min<int64_t>(best_ask_, pi);

        res.handle = n;
        return res;
    }

    // No owner (STP off), with a trade callback.
    template <class OnTrade>
        requires std::invocable<OnTrade&, const Trade&>
    AddResult add(uint64_t order_id, Side side, int64_t price_ticks, uint32_t qty,
                  OnTrade&& on_trade) {
        return add(order_id, side, price_ticks, qty, kNoOwner, on_trade);
    }

    // No trade callback.
    AddResult add(uint64_t order_id, Side side, int64_t price_ticks, uint32_t qty,
                  Owner owner = kNoOwner) {
        return add(order_id, side, price_ticks, qty, owner, [](const Trade&) {});
    }

    // Cancel by order id. Returns false if the id isn't a live order
    // (unknown, already cancelled, or out of range) - so double cancels are safe.
    bool cancel(uint64_t order_id) {
        if (order_id >= id_to_node_.size()) return false;
        Idx handle = id_to_node_[order_id];
        if (handle == NIL) return false;

        Node& o = pool_[handle];
        Level& lvl = ladder(o.side)[o.price_idx];
        unlink(lvl, handle);
        lvl.total_qty -= o.qty;
        lvl.count -= 1;
        pool_.free(handle);
        id_to_node_[order_id] = NIL;

        if (lvl.count == 0) {
            int64_t i = o.price_idx;
            if (o.side == Side::Buy && i == best_bid_) best_bid_ = next_bid_from(i - 1);
            if (o.side == Side::Sell && i == best_ask_) best_ask_ = next_ask_from(i + 1);
        }
        return true;
    }

    std::optional<int64_t> best_bid() const {
        if (best_bid_ < 0) return std::nullopt;
        return min_price_ + best_bid_;
    }
    std::optional<int64_t> best_ask() const {
        if (best_ask_ >= static_cast<int64_t>(num_levels_)) return std::nullopt;
        return min_price_ + best_ask_;
    }
    bool is_live(uint64_t order_id) const {
        return order_id < id_to_node_.size() && id_to_node_[order_id] != NIL;
    }

    const Level& level(Side side, int64_t price_ticks) const {
        return ladder(side)[static_cast<uint32_t>(price_ticks - min_price_)];
    }
    const Node& node(Idx handle) const { return pool_[handle]; }

private:
    std::vector<Level>&       ladder(Side s)       { return s == Side::Buy ? bids_ : asks_; }
    const std::vector<Level>& ladder(Side s) const { return s == Side::Buy ? bids_ : asks_; }

    // Fill against one price level, oldest order first. Returns shares left.
    // Sets self_trade and stops if the next maker has the taker's owner.
    template <class OnTrade>
    uint32_t match_level(std::vector<Level>& side_levels, uint32_t li,
                         uint64_t taker_id, Side taker_side, Owner taker_owner,
                         uint32_t remaining, bool& self_trade, OnTrade& on_trade) {
        Level& lvl = side_levels[li];
        int64_t price = min_price_ + li;
        while (remaining > 0 && lvl.head != NIL) {
            Idx m = lvl.head;
            Node& maker = pool_[m];
            if (taker_owner != kNoOwner && maker.owner == taker_owner) {
                self_trade = true;
                break;
            }
            uint32_t fill = std::min(remaining, maker.qty);

            on_trade(Trade{taker_id, maker.order_id, price, fill, taker_side});

            maker.qty     -= fill;
            lvl.total_qty -= fill;
            remaining     -= fill;

            if (maker.qty == 0) { // fully filled: remove and recycle
                unlink(lvl, m);
                lvl.count -= 1;
                id_to_node_[maker.order_id] = NIL;
                pool_.free(m);
            }
        }
        return remaining;
    }

    // Next non-empty level scanning down from i (bids) / up from i (asks).
    int64_t next_bid_from(int64_t i) const {
        while (i >= 0 && bids_[i].count == 0) --i;
        return i; // -1 if none
    }
    int64_t next_ask_from(int64_t i) const {
        while (i < static_cast<int64_t>(num_levels_) && asks_[i].count == 0) ++i;
        return i; // num_levels_ if none
    }

    // Append node n at the back of the level's queue.
    void link_tail(Level& lvl, Idx n) {
        Node& o = pool_[n];
        o.prev = lvl.tail;
        o.next = NIL;
        if (lvl.tail != NIL) pool_[lvl.tail].next = n;
        else                 lvl.head = n; // queue was empty
        lvl.tail = n;
    }

    // Remove node n from anywhere in the level's queue: O(1).
    void unlink(Level& lvl, Idx n) {
        Node& o = pool_[n];
        if (o.prev != NIL) pool_[o.prev].next = o.next;
        else               lvl.head = o.next; // n was the head
        if (o.next != NIL) pool_[o.next].prev = o.prev;
        else               lvl.tail = o.prev; // n was the tail
    }

    int64_t            min_price_;
    uint32_t           num_levels_;
    std::vector<Level> bids_;
    std::vector<Level> asks_;
    NodePool           pool_;
    std::vector<Idx>   id_to_node_; // order_id -> pool index, NIL if not live
    int64_t            best_bid_;   // level index, -1 if no bids
    int64_t            best_ask_;   // level index, num_levels_ if no asks
};
