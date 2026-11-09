// Benchmark: realistic-ish mixed order flow through
//   M) std::map<price, std::list<Order>> book with the same matching rules
//   B) OrderBook (flat array + intrusive lists + node pool + id array)
//
// Flow (pregenerated, same stream for both books):
//   - passive limit orders: prices clustered near a fixed mid (normal, sd ~10 ticks),
//     buys below mid, sells above, so they rest
//   - cancels: target a recent order id (may already be filled -> "too late to cancel")
//   - aggressive limit orders: cross the spread by a few ticks and trade, sometimes
//     sweeping several levels; any remainder rests
//
// Usage: bench_matching [num_ops=2000000] [pct_cancel=40] [pct_aggressive=10] [runs=5]
// Both books must produce identical trade counts and volume, or the run aborts.

#include "order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <list>
#include <map>
#include <random>
#include <unordered_map>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int64_t  kMinPrice  = 10000;
constexpr uint32_t kNumLevels = 1000;
constexpr int64_t  kMid       = 10500;

enum class OpType : uint8_t { Add, Cancel };

struct Op {
    OpType   type;
    Side     side;
    uint64_t id;
    int64_t  price;
    uint32_t qty;
};

struct Stats {
    uint64_t trades = 0;
    uint64_t volume = 0;
};

// ---------------------------------------------------------------------------
// Baseline: std::map of price -> std::list of orders, std::unordered_map for
// id -> location. Same price-time priority rules as OrderBook.
// ---------------------------------------------------------------------------
class MapBook {
public:
    explicit MapBook(size_t expected_ids) { ids_.reserve(expected_ids); }

    void add(uint64_t id, Side side, int64_t price, uint32_t qty, Stats& st) {
        if (side == Side::Buy) {
            qty = match(asks_, [&](int64_t best) { return best <= price; }, qty, st);
            if (qty) rest(bids_, id, side, price, qty);
        } else {
            qty = match(bids_, [&](int64_t best) { return best >= price; }, qty, st);
            if (qty) rest(asks_, id, side, price, qty);
        }
    }

    bool cancel(uint64_t id) {
        auto it = ids_.find(id);
        if (it == ids_.end()) return false;
        Loc loc = it->second;
        ids_.erase(it);
        if (loc.side == Side::Buy) erase_from(bids_, loc);
        else                       erase_from(asks_, loc);
        return true;
    }

private:
    struct Order { uint64_t id; uint32_t qty; };
    using Queue = std::list<Order>;
    struct Loc { Side side; int64_t price; Queue::iterator it; };

    template <class Map, class Crosses>
    uint32_t match(Map& opp, Crosses crosses, uint32_t qty, Stats& st) {
        while (qty > 0 && !opp.empty() && crosses(opp.begin()->first)) {
            Queue& q = opp.begin()->second;
            while (qty > 0 && !q.empty()) {
                Order& maker = q.front();
                uint32_t fill = std::min(qty, maker.qty);
                st.trades += 1;
                st.volume += fill;
                maker.qty -= fill;
                qty -= fill;
                if (maker.qty == 0) { ids_.erase(maker.id); q.pop_front(); }
            }
            if (q.empty()) opp.erase(opp.begin());
        }
        return qty;
    }

    template <class Map>
    void rest(Map& book, uint64_t id, Side side, int64_t price, uint32_t qty) {
        Queue& q = book[price];
        q.push_back(Order{id, qty});
        ids_.emplace(id, Loc{side, price, std::prev(q.end())});
    }

    template <class Map>
    void erase_from(Map& book, const Loc& loc) {
        auto lvl = book.find(loc.price);
        lvl->second.erase(loc.it);
        if (lvl->second.empty()) book.erase(lvl);
    }

    std::map<int64_t, Queue, std::greater<>> bids_; // begin() = highest bid
    std::map<int64_t, Queue>                 asks_; // begin() = lowest ask
    std::unordered_map<uint64_t, Loc>        ids_;
};

// ---------------------------------------------------------------------------

struct Flow {
    std::vector<Op> ops;
    uint64_t num_adds = 0, num_cancels = 0, num_aggressive = 0;
};

Flow make_flow(size_t n, int pct_cancel, int pct_aggr, uint64_t seed) {
    Flow f;
    f.ops.reserve(n);
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> pct(0, 99);
    std::normal_distribution<double> dist_from_mid(0.0, 10.0);
    std::uniform_int_distribution<int> lots(1, 5);     // 100..500 shares passive
    std::uniform_int_distribution<int> big_lots(2, 10); // 200..1000 shares aggressive
    std::uniform_int_distribution<int> cross_by(0, 5);  // ticks past the mid
    std::uniform_int_distribution<int> recent(0, 999);  // cancel one of the last 1000 adds

    uint64_t next_id = 0;
    for (size_t i = 0; i < n; ++i) {
        int r = pct(rng);
        Side side = (rng() & 1) ? Side::Buy : Side::Sell;

        if (r < pct_cancel && next_id > 0) {
            uint64_t back = std::min<uint64_t>(next_id, recent(rng) + 1);
            f.ops.push_back(Op{OpType::Cancel, side, next_id - back, 0, 0});
            f.num_cancels++;
        } else if (r < pct_cancel + pct_aggr) {
            int64_t k = cross_by(rng);
            int64_t price = side == Side::Buy ? kMid + k : kMid - k;
            f.ops.push_back(Op{OpType::Add, side, next_id++, price,
                               static_cast<uint32_t>(100 * big_lots(rng))});
            f.num_adds++;
            f.num_aggressive++;
        } else {
            int64_t off = 1 + static_cast<int64_t>(std::abs(dist_from_mid(rng)));
            int64_t price = side == Side::Buy ? kMid - off : kMid + off;
            price = std::clamp<int64_t>(price, kMinPrice, kMinPrice + kNumLevels - 1);
            f.ops.push_back(Op{OpType::Add, side, next_id++, price,
                               static_cast<uint32_t>(100 * lots(rng))});
            f.num_adds++;
        }
    }
    return f;
}

double run_map(const Flow& f, Stats& st) {
    MapBook book(f.num_adds);
    auto t0 = Clock::now();
    for (const Op& op : f.ops) {
        if (op.type == OpType::Add) book.add(op.id, op.side, op.price, op.qty, st);
        else                        book.cancel(op.id);
    }
    auto t1 = Clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

double run_flat(const Flow& f, Stats& st) {
    OrderBook book(kMinPrice, kNumLevels, static_cast<Idx>(f.num_adds), f.num_adds);
    auto on_trade = [&st](const Trade& t) { st.trades += 1; st.volume += t.qty; };
    auto t0 = Clock::now();
    for (const Op& op : f.ops) {
        if (op.type == OpType::Add) book.add(op.id, op.side, op.price, op.qty, on_trade);
        else                        book.cancel(op.id);
    }
    auto t1 = Clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

} // namespace

int main(int argc, char** argv) {
    size_t n        = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2'000'000;
    int pct_cancel  = argc > 2 ? std::atoi(argv[2]) : 40;
    int pct_aggr    = argc > 3 ? std::atoi(argv[3]) : 10;
    int runs        = argc > 4 ? std::atoi(argv[4]) : 5;

    Flow f = make_flow(n, pct_cancel, pct_aggr, 42);
    std::printf("ops=%zu  adds=%llu (aggressive=%llu)  cancels=%llu  levels=%u\n", n,
                (unsigned long long)f.num_adds, (unsigned long long)f.num_aggressive,
                (unsigned long long)f.num_cancels, kNumLevels);

    double best_map = 1e30, best_flat = 1e30;
    Stats sm, sf;
    for (int r = 0; r < runs; ++r) {
        sm = {}; sf = {};
        double m = run_map(f, sm);
        double b = run_flat(f, sf);
        if (sm.trades != sf.trades || sm.volume != sf.volume) {
            std::printf("MISMATCH: map trades=%llu vol=%llu, flat trades=%llu vol=%llu\n",
                        (unsigned long long)sm.trades, (unsigned long long)sm.volume,
                        (unsigned long long)sf.trades, (unsigned long long)sf.volume);
            return 1;
        }
        std::printf("  run %d: map %7.2f ns/op   flat %6.2f ns/op\n", r, m / n, b / n);
        best_map  = std::min(best_map, m);
        best_flat = std::min(best_flat, b);
    }
    std::printf("trades=%llu  volume=%llu  (identical in both books)\n",
                (unsigned long long)sf.trades, (unsigned long long)sf.volume);
    std::printf("best: map %.2f ns/op, flat %.2f ns/op, ratio %.1fx\n",
                best_map / n, best_flat / n, best_map / best_flat);
}
