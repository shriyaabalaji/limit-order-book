#pragma once
// Perpetual futures layer on top of OrderBook (one market, e.g. BTC-PERP).
//
// Units: prices are integer ticks (cents), sizes are contracts, money is
// ticks x contracts (cents, since 1 contract = 1 unit of the underlying).
// No floating point anywhere in the accounting.
//
// Positions: each account has a signed size (+ long, - short) and `cost`,
// the signed entry notional of the open position. Entry price = cost / size.
//   - adding to a position:  cost += price * qty    (weighted-average entry)
//   - reducing a position:   remove the closed share of cost, and
//                            realized PnL = exit notional - cost removed
//   - a fill bigger than the position closes it and opens the other way
//
// Mark price: mid of best bid and best ask. If one side of the book is
// empty, keep the last mark (nullopt until both sides have existed once).
//
// Equity = collateral + unrealized, where unrealized = mark * size - cost.
// Rounding in `cost` only moves cents between realized and unrealized;
// equity itself is exact, so (sum of all equity) == (sum of all deposits)
// + (bad debt written off). Every fill and funding payment is zero-sum.
//
// Initial margin (checked before an order enters the book):
//   worst-case size = the bigger of |size + all open buys| and
//                     |size - all open sells|, counting the new order as if
//                     it fully fills.
//   required = |worst-case size| * reference price * im_bps / 10000
//   The order is rejected if it raises worst-case size AND equity < required.
//   An order that doesn't raise worst-case size (e.g. selling while long)
//   is always allowed, so you can always get out of a position.
//   Reference price = mark, or the order's own price if there's no mark yet.
//
// Liquidation (after every order, cancel and funding payment):
//   if equity < |size| * mark * mm_bps / 10000, the account's open orders are
//   cancelled and its whole position is sent into the book as a market order
//   (IOC: whatever doesn't fill is cancelled, and we retry on the next event).
//   That order can move the mark and push other accounts under, so we loop
//   until nobody is liquidatable - a liquidation cascade.
//   If the account ends flat with negative collateral, that's bad debt: the
//   winners were paid money the loser didn't have. We zero the account and
//   record it (a real exchange covers it from an insurance fund).
//
// Funding (apply_funding, called every interval with an external index price):
//   per-contract payment = mark - index, capped at +/- funding_cap_bps of index.
//   Longs pay shorts when the perp trades above the index, and vice versa,
//   which pulls the perp's price back toward spot. Rounded to whole ticks per
//   contract so the payments still sum to exactly zero.
//
// Not done: insurance fund, auto-deleveraging, liquidation fees, cross-margin.

#include "order_book.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

struct Account {
    int64_t collateral = 0; // deposits + realized PnL + funding received
    int64_t realized   = 0; // cumulative realized PnL from trading
    int64_t funding    = 0; // cumulative funding received (negative = paid)
    int64_t size       = 0; // contracts: + long, - short
    int64_t cost       = 0; // signed entry notional of the open position
    int64_t open_buy   = 0; // contracts resting in the book on the bid side
    int64_t open_sell  = 0; // contracts resting in the book on the ask side
    uint32_t liquidations = 0;
};

struct PlaceResult {
    AddResult book;                    // what the order book did
    uint64_t  id = 0;                  // exchange-assigned order id
    bool      margin_rejected = false; // not enough equity for initial margin
};

struct Liquidation {
    Owner   who;
    int64_t size;      // position before liquidation (signed)
    int64_t closed;    // contracts actually closed in the book
    int64_t bad_debt;  // loss the account couldn't cover (0 if none)
};

struct PerpConfig {
    int64_t  min_price_ticks = 9000;
    uint32_t num_levels      = 2001;
    Idx      max_orders      = 20000;
    uint64_t max_order_id    = 1'000'000;
    Owner    max_accounts    = 64;     // owners 1..max_accounts-1
    int64_t  im_bps          = 1000;   // initial margin 10% -> max 10x
    int64_t  mm_bps          = 500;    // maintenance margin 5%
    int64_t  funding_cap_bps = 50;     // funding capped at 0.5% per interval
};

class PerpExchange {
public:
    explicit PerpExchange(const PerpConfig& cfg)
        : cfg_(cfg),
          book_(cfg.min_price_ticks, cfg.num_levels, cfg.max_orders, cfg.max_order_id),
          accounts_(cfg.max_accounts),
          open_ids_(cfg.max_accounts) {}

    bool deposit(Owner who, int64_t amount) {
        if (!valid(who) || amount <= 0) return false;
        accounts_[who].collateral += amount;
        deposits_ += amount;
        return true;
    }

    // Limit order for `who`. Every fill settles both sides. `on_trade` sees
    // each fill, including fills from any liquidations this order triggers.
    template <class OnTrade>
        requires std::invocable<OnTrade&, const Trade&>
    PlaceResult place(Owner who, Side side, int64_t price_ticks, uint32_t qty,
                      OnTrade&& on_trade) {
        PlaceResult res;
        if (!valid(who)) return res;

        // 1. Initial margin check, assuming the whole order fills.
        const Account& a = accounts_[who];
        int64_t q = qty;
        int64_t before = worst_case_size(a, 0, 0);
        int64_t after  = side == Side::Buy ? worst_case_size(a, q, 0) : worst_case_size(a, 0, q);
        if (after > before && equity(who) < margin(after, mark_.value_or(price_ticks), cfg_.im_bps)) {
            res.margin_rejected = true;
            return res;
        }

        // 2. Send it to the book, then let the new mark liquidate whoever it should.
        res = submit(who, side, price_ticks, qty, on_trade);
        refresh_mark();
        run_liquidations(on_trade);
        return res;
    }

    PlaceResult place(Owner who, Side side, int64_t price_ticks, uint32_t qty) {
        return place(who, side, price_ticks, qty, [](const Trade&) {});
    }

    // Only the order's owner can cancel it. Pulling a quote can move the
    // mark, so this can trigger liquidations too.
    template <class OnTrade>
        requires std::invocable<OnTrade&, const Trade&>
    bool cancel(Owner who, uint64_t order_id, OnTrade&& on_trade) {
        if (!cancel_order(who, order_id)) return false;
        refresh_mark();
        run_liquidations(on_trade);
        return true;
    }

    bool cancel(Owner who, uint64_t order_id) {
        return cancel(who, order_id, [](const Trade&) {});
    }

    // Pay funding for one interval against an external index (spot) price.
    // Returns the per-contract payment from longs to shorts (negative:
    // shorts pay longs).
    template <class OnTrade>
        requires std::invocable<OnTrade&, const Trade&>
    int64_t apply_funding(int64_t index_ticks, OnTrade&& on_trade) {
        if (!mark_) return 0;
        int64_t cap = index_ticks * cfg_.funding_cap_bps / 10000;
        int64_t per_contract = std::clamp(*mark_ - index_ticks, -cap, cap);
        if (per_contract == 0) return 0;
        for (Account& acct : accounts_) {
            int64_t pay = acct.size * per_contract; // longs pay when positive
            acct.collateral -= pay;
            acct.funding    -= pay;
        }
        run_liquidations(on_trade);
        return per_contract;
    }

    int64_t apply_funding(int64_t index_ticks) {
        return apply_funding(index_ticks, [](const Trade&) {});
    }

    std::optional<int64_t> mark() const { return mark_; }

    // Unrealized PnL at the current mark (0 if there's no mark yet).
    int64_t unrealized(Owner who) const {
        const Account& a = accounts_[who];
        return mark_ ? *mark_ * a.size - a.cost : 0;
    }
    int64_t equity(Owner who) const { return accounts_[who].collateral + unrealized(who); }

    int64_t maintenance_margin(Owner who) const {
        return mark_ ? margin(std::abs(accounts_[who].size), *mark_, cfg_.mm_bps) : 0;
    }
    int64_t initial_margin_used(Owner who) const {
        const Account& a = accounts_[who];
        return mark_ ? margin(worst_case_size(a, 0, 0), *mark_, cfg_.im_bps) : 0;
    }

    // Display only: average entry in ticks.
    double entry_price(Owner who) const {
        const Account& a = accounts_[who];
        return a.size ? static_cast<double>(a.cost) / static_cast<double>(a.size) : 0.0;
    }

    // Display only: mark at which equity == maintenance margin, solving
    // collateral + P*size - cost = mm * |size| * P  for P. Ignores open
    // orders and future funding. 0 if flat.
    double liquidation_price(Owner who) const {
        const Account& a = accounts_[who];
        if (a.size == 0) return 0.0;
        double mm = static_cast<double>(cfg_.mm_bps) / 10000.0;
        double s  = static_cast<double>(a.size);
        return static_cast<double>(a.cost - a.collateral) / (s - mm * std::abs(s));
    }

    // Liquidations since the last call (for a UI log).
    std::vector<Liquidation> take_liquidations() {
        std::vector<Liquidation> out;
        out.swap(liquidations_);
        return out;
    }

    const Account&    account(Owner who) const { return accounts_[who]; }
    const OrderBook&  book() const { return book_; }
    const PerpConfig& config() const { return cfg_; }
    int64_t total_deposits() const { return deposits_; }
    bool    liquidating() const { return liquidating_; } // true inside on_trade for liquidation fills
    int64_t bad_debt() const { return bad_debt_; }

private:
    bool valid(Owner who) const { return who != kNoOwner && who < accounts_.size(); }

    // Largest position you could end up with if every open order (plus the
    // extra buy/sell qty) filled. Buys and sells can't both push you further
    // out, so it's the worse of the two directions.
    static int64_t worst_case_size(const Account& a, int64_t extra_buy, int64_t extra_sell) {
        return std::max(std::abs(a.size + a.open_buy + extra_buy),
                        std::abs(a.size - a.open_sell - extra_sell));
    }

    static int64_t margin(int64_t size, int64_t price, int64_t bps) {
        return size * price * bps / 10000;
    }

    bool below_maintenance(Owner who) const {
        return accounts_[who].size != 0 && mark_ && equity(who) < maintenance_margin(who);
    }

    // Book add + settlement + open-order bookkeeping. No margin check, no
    // mark refresh, no liquidations.
    template <class OnTrade>
    PlaceResult submit(Owner who, Side side, int64_t price_ticks, uint32_t qty, OnTrade& on_trade) {
        PlaceResult res;
        if (next_id_ >= cfg_.max_order_id) return res;
        res.id = next_id_;
        Account& a = accounts_[who];
        int64_t taker_sign = side == Side::Buy ? 1 : -1;
        res.book = book_.add(res.id, side, price_ticks, qty, who, [&](const Trade& t) {
            int64_t fq = t.qty;
            Account& maker = accounts_[t.maker_owner];
            apply_fill(a, taker_sign * fq, t.price_ticks);
            apply_fill(maker, -taker_sign * fq, t.price_ticks);
            (side == Side::Buy ? maker.open_sell : maker.open_buy) -= fq; // maker's resting qty shrank
            on_trade(t);
        });
        if (!res.book.accepted) return res;
        ++next_id_;
        if (res.book.handle != NIL) { // the remainder rested
            (side == Side::Buy ? a.open_buy : a.open_sell) += book_.node(res.book.handle).qty;
            remember_order(who, res.id);
        }
        return res;
    }

    bool cancel_order(Owner who, uint64_t order_id) {
        const Node* n = book_.find(order_id);
        if (!n || n->owner != who) return false;
        Account& a = accounts_[who];
        (n->side == Side::Buy ? a.open_buy : a.open_sell) -= n->qty;
        book_.cancel(order_id);
        return true;
    }

    // Track resting order ids per account so a liquidation can cancel them.
    // Filled/cancelled ids are pruned lazily.
    void remember_order(Owner who, uint64_t id) {
        auto& ids = open_ids_[who];
        if (ids.size() >= 64) std::erase_if(ids, [&](uint64_t x) { return !book_.is_live(x); });
        ids.push_back(id);
    }

    template <class OnTrade>
    void run_liquidations(OnTrade& on_trade) {
        if (liquidating_) return; // a liquidation's own order doesn't recurse
        liquidating_ = true;
        // Each pass can move the mark and push more accounts under. Bounded,
        // because a pass that can't fill anything (empty book side) stops.
        for (int pass = 0; pass < 100; ++pass) {
            bool any = false;
            for (Owner o = 1; o < accounts_.size(); ++o)
                if (below_maintenance(o) && liquidate(o, on_trade)) any = true;
            if (!any) break;
        }
        liquidating_ = false;
    }

    // Returns true if any of the position was closed.
    template <class OnTrade>
    bool liquidate(Owner who, OnTrade& on_trade) {
        Account& a = accounts_[who];
        Side side = a.size > 0 ? Side::Sell : Side::Buy;
        if (!(side == Side::Sell ? book_.best_bid() : book_.best_ask())) return false; // nobody to sell to

        for (uint64_t id : open_ids_[who]) cancel_order(who, id); // dead ids just return false
        open_ids_[who].clear();

        int64_t before = a.size;
        // Market order: limit at the far end of the ladder, then cancel any remainder (IOC).
        int64_t px = side == Side::Sell ? cfg_.min_price_ticks
                                        : cfg_.min_price_ticks + cfg_.num_levels - 1;
        PlaceResult r = submit(who, side, px, static_cast<uint32_t>(std::abs(before)), on_trade);
        if (r.book.handle != NIL) cancel_order(who, r.id);
        refresh_mark();

        Liquidation ev{who, before, before - a.size, 0};
        if (a.size == 0 && a.collateral < 0) { // flat but owes money: bad debt
            ev.bad_debt = -a.collateral;
            bad_debt_  += ev.bad_debt;
            a.collateral = 0;
        }
        ++a.liquidations;
        liquidations_.push_back(ev);
        return ev.closed != 0;
    }

    // Apply a fill of `dq` contracts (+ bought, - sold) at `price` to one account.
    static void apply_fill(Account& a, int64_t dq, int64_t price) {
        // 1. If the fill is against the current position, close part (or all) of it.
        if (a.size != 0 && (a.size > 0) != (dq > 0)) {
            int64_t close = std::min(std::abs(dq), std::abs(a.size));
            if (a.size < 0) close = -close;                 // same sign as the position
            int64_t cost_removed = a.cost * close / a.size; // closed share of entry notional
            int64_t pnl = price * close - cost_removed;
            a.collateral += pnl;
            a.realized   += pnl;
            a.size -= close;
            a.cost -= cost_removed;
            dq += close;                                    // what's left of the fill
        }
        // 2. Anything left opens / adds to a position in the fill's direction.
        if (dq != 0) {
            a.size += dq;
            a.cost += price * dq;
        }
    }

    void refresh_mark() {
        auto bid = book_.best_bid(), ask = book_.best_ask();
        if (bid && ask) mark_ = (*bid + *ask) / 2; // rounds down by up to half a tick
    }

    PerpConfig                         cfg_;
    OrderBook                          book_;
    std::vector<Account>               accounts_;
    std::vector<std::vector<uint64_t>> open_ids_;
    std::vector<Liquidation>           liquidations_;
    std::optional<int64_t>             mark_;
    uint64_t                           next_id_ = 0;
    int64_t                            deposits_ = 0;
    int64_t                            bad_debt_ = 0;
    bool                               liquidating_ = false;
};
