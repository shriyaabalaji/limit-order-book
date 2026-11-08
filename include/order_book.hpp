#pragma once
// Base limit order book: flat price array + intrusive doubly linked lists
// + preallocated node pool with an intrusive free list.
//
// Not done yet (yours to design):
//   - order_id -> handle lookup (cancel currently takes the handle from add())
//   - matching
//   - tracking best bid / best ask
//   - what to do on out-of-range prices / pool exhaustion (currently: return NIL)

#include <cstdint>
#include <vector>

using Idx = uint32_t;                 // index into the node pool
inline constexpr Idx NIL = UINT32_MAX; // "no node"

enum class Side : uint8_t { Buy, Sell };

// One resting order. Lives in the pool. `prev`/`next` link it into its
// price level's FIFO queue while in use, and `next` links it into the
// free list while unused.
struct Node {
    uint64_t order_id;
    uint32_t qty;
    uint32_t price_idx; // index into the ladder, so cancel can find its level
    Idx      prev;
    Idx      next;
    Side     side;
};

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

// ---------------------------------------------------------------------------
// Order book. Prices are integer ticks (no floating point).
// Slot i in each ladder represents price (min_price_ticks + i).
// ---------------------------------------------------------------------------
class OrderBook {
public:
    OrderBook(int64_t min_price_ticks, uint32_t num_levels, Idx max_orders)
        : min_price_(min_price_ticks),
          num_levels_(num_levels),
          bids_(num_levels),
          asks_(num_levels),
          pool_(max_orders) {}

    // Returns a handle (pool index) for later cancel, or NIL if rejected.
    Idx add(uint64_t order_id, Side side, int64_t price_ticks, uint32_t qty) {
        int64_t offset = price_ticks - min_price_;
        if (offset < 0 || offset >= static_cast<int64_t>(num_levels_)) return NIL;

        Idx n = pool_.alloc();
        if (n == NIL) return NIL;

        auto pi = static_cast<uint32_t>(offset);
        pool_[n] = Node{order_id, qty, pi, NIL, NIL, side};

        Level& lvl = ladder(side)[pi];
        link_tail(lvl, n);
        lvl.total_qty += qty;
        lvl.count += 1;
        return n;
    }

    // `handle` must be a live order returned by add().
    void cancel(Idx handle) {
        Node& o = pool_[handle];
        Level& lvl = ladder(o.side)[o.price_idx];
        unlink(lvl, handle);
        lvl.total_qty -= o.qty;
        lvl.count -= 1;
        pool_.free(handle);
    }

    const Level& level(Side side, int64_t price_ticks) const {
        return ladder(side)[static_cast<uint32_t>(price_ticks - min_price_)];
    }
    const Node& node(Idx handle) const { return pool_[handle]; }

private:
    std::vector<Level>&       ladder(Side s)       { return s == Side::Buy ? bids_ : asks_; }
    const std::vector<Level>& ladder(Side s) const { return s == Side::Buy ? bids_ : asks_; }

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
};
