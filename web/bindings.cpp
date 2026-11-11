// WebAssembly bindings: exposes the real OrderBook to JavaScript via embind.
// Build: web/build.sh

#include "order_book.hpp"
#include "perp.hpp"

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <vector>

using emscripten::val;

namespace {

constexpr int64_t  kMinPrice  = 9000;  // $90.00 in cents
constexpr uint32_t kNumLevels = 2001;  // $90.00 .. $110.00, 1 cent ticks
constexpr Idx      kMaxOrders = 20000;
constexpr uint64_t kMaxIds    = 1'000'000;

const char* reason_str(CancelReason r) {
    switch (r) {
        case CancelReason::SelfTrade: return "self-trade";
        case CancelReason::PoolFull:  return "pool-full";
        default:                      return "";
    }
}


// Top `n` non-empty levels on a side, best first: [{price, qty, count}]
val depth_of(const OrderBook& book, int side, int n) {
    val arr = val::array();
    Side s = side == 0 ? Side::Buy : Side::Sell;
    auto best = s == Side::Buy ? book.best_bid() : book.best_ask();
    if (!best) return arr;
    int step = s == Side::Buy ? -1 : 1;
    int found = 0;
    for (int64_t p = *best; p >= kMinPrice && p < kMinPrice + kNumLevels && found < n; p += step) {
        const Level& lvl = book.level(s, p);
        if (lvl.count == 0) continue;
        val lv = val::object();
        lv.set("price", static_cast<double>(p));
        lv.set("qty", static_cast<double>(lvl.total_qty));
        lv.set("count", lvl.count);
        arr.call<void>("push", lv);
        ++found;
    }
    return arr;
}

class Playground {
public:
    Playground() : book_(kMinPrice, kNumLevels, kMaxOrders, kMaxIds) {}

    int64_t minPrice() const { return kMinPrice; }
    int64_t maxPrice() const { return kMinPrice + kNumLevels - 1; }

    // side: 0 = buy, 1 = sell. Returns {accepted, id, filled, cancelled, reason, rested, trades[]}.
    val place(int side, double price, double qty, double owner) {
        Side s = side == 0 ? Side::Buy : Side::Sell;
        uint64_t id = next_id_++;
        std::vector<Trade> trades;
        AddResult r = book_.add(id, s, static_cast<int64_t>(price), static_cast<uint32_t>(qty),
                                static_cast<Owner>(owner),
                                [&](const Trade& t) { trades.push_back(t); });

        val out = val::object();
        out.set("accepted", r.accepted);
        out.set("id", static_cast<double>(id));
        out.set("filled", r.filled);
        out.set("cancelled", r.cancelled);
        out.set("reason", std::string(reason_str(r.reason)));
        out.set("rested", r.handle != NIL);

        val arr = val::array();
        for (const Trade& t : trades) {
            val tv = val::object();
            tv.set("takerId", static_cast<double>(t.taker_id));
            tv.set("makerId", static_cast<double>(t.maker_id));
            tv.set("price", static_cast<double>(t.price_ticks));
            tv.set("qty", t.qty);
            tv.set("takerSide", t.taker_side == Side::Buy ? 0 : 1);
            arr.call<void>("push", tv);
        }
        out.set("trades", arr);
        return out;
    }

    bool cancel(double id) { return book_.cancel(static_cast<uint64_t>(id)); }

    // Remaining resting qty for an order id, or 0 if it isn't live.
    double remaining(double id) const {
        const Node* n = book_.find(static_cast<uint64_t>(id));
        return n ? n->qty : 0;
    }

    val bestBid() const { auto b = book_.best_bid(); return b ? val(static_cast<double>(*b)) : val::null(); }
    val bestAsk() const { auto a = book_.best_ask(); return a ? val(static_cast<double>(*a)) : val::null(); }

    // Top `n` non-empty levels on a side, best first: [{price, qty, count}]
    val depth(int side, int n) const { return depth_of(book_, side, n); }

private:
    OrderBook book_;
    uint64_t  next_id_ = 1;
};


// ---------------------------------------------------------------------------
// Perp playground: the same book wrapped in PerpExchange (positions, margin,
// liquidation, funding). Every mutating call returns the trades and
// liquidations it caused, since one order can set off a cascade.
// ---------------------------------------------------------------------------
// Collects trades (flagged if they came from a liquidation) and liquidations.
struct Events {
    PerpExchange& ex;
    val trades = val::array();
    explicit Events(PerpExchange& e) : ex(e) {}
    auto cb() {
        return [this](const Trade& t) {
            val tv = val::object();
            tv.set("price", static_cast<double>(t.price_ticks));
            tv.set("qty", t.qty);
            tv.set("takerSide", t.taker_side == Side::Buy ? 0 : 1);
            tv.set("takerOwner", t.taker_owner);
            tv.set("makerOwner", t.maker_owner);
            tv.set("makerId", static_cast<double>(t.maker_id));
            tv.set("liquidation", ex.liquidating());
            trades.call<void>("push", tv);
        };
    }
    val finish() {
        val out = val::object();
        out.set("trades", trades);
        val liqs = val::array();
        for (const Liquidation& l : ex.take_liquidations()) {
            val lv = val::object();
            lv.set("who", l.who);
            lv.set("size", static_cast<double>(l.size));
            lv.set("closed", static_cast<double>(l.closed));
            lv.set("badDebt", static_cast<double>(l.bad_debt));
            liqs.call<void>("push", lv);
        }
        out.set("liquidations", liqs);
        return out;
    }
};

class PerpPlayground {
public:
    PerpPlayground() : ex_(make_cfg()) {}

    bool deposit(double owner, double cents) {
        return ex_.deposit(static_cast<Owner>(owner), static_cast<int64_t>(cents));
    }

    val place(double owner, int side, double price, double qty) {
        Events ev(ex_);
        PlaceResult r = ex_.place(static_cast<Owner>(owner), side == 0 ? Side::Buy : Side::Sell,
                                  static_cast<int64_t>(price), static_cast<uint32_t>(qty), ev.cb());
        val out = ev.finish();
        out.set("accepted", r.book.accepted);
        out.set("marginRejected", r.margin_rejected);
        out.set("id", static_cast<double>(r.id));
        out.set("filled", r.book.filled);
        out.set("cancelled", r.book.cancelled);
        out.set("reason", std::string(reason_str(r.book.reason)));
        out.set("rested", r.book.handle != NIL);
        return out;
    }

    val cancel(double owner, double id) {
        Events ev(ex_);
        bool ok = ex_.cancel(static_cast<Owner>(owner), static_cast<uint64_t>(id), ev.cb());
        val out = ev.finish();
        out.set("ok", ok);
        return out;
    }

    // One funding interval against `index` (cents). perContract > 0: longs paid shorts.
    val funding(double index) {
        Events ev(ex_);
        int64_t f = ex_.apply_funding(static_cast<int64_t>(index), ev.cb());
        val out = ev.finish();
        out.set("perContract", static_cast<double>(f));
        return out;
    }

    val account(double owner) const {
        auto o = static_cast<Owner>(owner);
        const Account& a = ex_.account(o);
        val v = val::object();
        v.set("collateral", static_cast<double>(a.collateral));
        v.set("realized", static_cast<double>(a.realized));
        v.set("funding", static_cast<double>(a.funding));
        v.set("size", static_cast<double>(a.size));
        v.set("entry", ex_.entry_price(o));
        v.set("unrealized", static_cast<double>(ex_.unrealized(o)));
        v.set("equity", static_cast<double>(ex_.equity(o)));
        v.set("maintenance", static_cast<double>(ex_.maintenance_margin(o)));
        v.set("initialUsed", static_cast<double>(ex_.initial_margin_used(o)));
        v.set("liqPrice", ex_.liquidation_price(o));
        v.set("liquidations", a.liquidations);
        return v;
    }

    val mark() const { auto m = ex_.mark(); return m ? val(static_cast<double>(*m)) : val::null(); }
    val bestBid() const { auto b = ex_.book().best_bid(); return b ? val(static_cast<double>(*b)) : val::null(); }
    val bestAsk() const { auto a = ex_.book().best_ask(); return a ? val(static_cast<double>(*a)) : val::null(); }
    double badDebt() const { return static_cast<double>(ex_.bad_debt()); }
    double imBps() const { return static_cast<double>(ex_.config().im_bps); }
    double mmBps() const { return static_cast<double>(ex_.config().mm_bps); }
    double remaining(double id) const {
        const Node* n = ex_.book().find(static_cast<uint64_t>(id));
        return n ? n->qty : 0;
    }
    val depth(int side, int n) const { return depth_of(ex_.book(), side, n); }

private:
    static PerpConfig make_cfg() {
        PerpConfig c;
        c.min_price_ticks = kMinPrice;
        c.num_levels      = kNumLevels;
        c.max_orders      = kMaxOrders;
        c.max_order_id    = kMaxIds * 4;
        c.max_accounts    = 64;
        return c;
    }

    PerpExchange ex_;
};

} // namespace

EMSCRIPTEN_BINDINGS(lob) {
    emscripten::class_<Playground>("Playground")
        .constructor<>()
        .function("minPrice", &Playground::minPrice)
        .function("maxPrice", &Playground::maxPrice)
        .function("place", &Playground::place)
        .function("cancel", &Playground::cancel)
        .function("remaining", &Playground::remaining)
        .function("bestBid", &Playground::bestBid)
        .function("bestAsk", &Playground::bestAsk)
        .function("depth", &Playground::depth);

    emscripten::class_<PerpPlayground>("PerpPlayground")
        .constructor<>()
        .function("deposit", &PerpPlayground::deposit)
        .function("place", &PerpPlayground::place)
        .function("cancel", &PerpPlayground::cancel)
        .function("funding", &PerpPlayground::funding)
        .function("account", &PerpPlayground::account)
        .function("mark", &PerpPlayground::mark)
        .function("bestBid", &PerpPlayground::bestBid)
        .function("bestAsk", &PerpPlayground::bestAsk)
        .function("badDebt", &PerpPlayground::badDebt)
        .function("imBps", &PerpPlayground::imBps)
        .function("mmBps", &PerpPlayground::mmBps)
        .function("remaining", &PerpPlayground::remaining)
        .function("depth", &PerpPlayground::depth);
}
