// WebAssembly bindings: exposes the real OrderBook to JavaScript via embind.
// Build: web/build.sh

#include "order_book.hpp"

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
    val depth(int side, int n) const {
        val arr = val::array();
        Side s = side == 0 ? Side::Buy : Side::Sell;
        auto best = s == Side::Buy ? book_.best_bid() : book_.best_ask();
        if (!best) return arr;
        int step = s == Side::Buy ? -1 : 1;
        int found = 0;
        for (int64_t p = *best; p >= kMinPrice && p <= maxPrice() && found < n; p += step) {
            const Level& lvl = book_.level(s, p);
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

private:
    OrderBook book_;
    uint64_t  next_id_ = 1;
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
}
