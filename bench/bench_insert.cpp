// Benchmark: insert N orders into
//   A) std::map<price, std::list<Order>>
//   B) OrderBook (flat price array + intrusive lists + node pool)
//   S) WK Selph (2011) design: tree of limits + price hash + heap orders + id hash
//   C) B plus an unordered_map<id, handle>, so it pays for id lookup like S does
//
// Hash maps are reserve()d up front (outside timing) for S and C.
// Usage: bench_insert [num_orders=1000000] [num_levels=1000] [runs=5]
// Prices are pregenerated (uniform over num_levels) so the RNG isn't timed.
// Container construction/destruction is excluded from timing.

#include "order_book.hpp"
#include "selph_book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <map>
#include <random>
#include <unordered_map>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Order {
    uint64_t order_id;
    uint32_t qty;
    Side     side;
};

// Prevent the compiler from optimizing away results.
volatile uint64_t g_sink;

constexpr int64_t kMinPrice = 23000;

double bench_map(const std::vector<int64_t>& prices) {
    std::map<int64_t, std::list<Order>> book;
    auto t0 = Clock::now();
    for (size_t i = 0; i < prices.size(); ++i) {
        book[prices[i]].push_back(Order{i, 100, Side::Buy});
    }
    auto t1 = Clock::now();
    g_sink = book.size() + book.begin()->second.size();
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
    // `book` is destroyed here, after timing.
}

double bench_flat(const std::vector<int64_t>& prices, uint32_t num_levels) {
    OrderBook book(kMinPrice, num_levels, static_cast<Idx>(prices.size()));
    uint64_t check = 0;
    auto t0 = Clock::now();
    for (size_t i = 0; i < prices.size(); ++i) {
        check += book.add(i, Side::Buy, prices[i], 100);
    }
    auto t1 = Clock::now();
    g_sink = check + book.level(Side::Buy, prices[0]).count;
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

double bench_selph(const std::vector<int64_t>& prices, uint32_t num_levels) {
    selph::Book book;
    book.reserve(prices.size(), num_levels);
    auto t0 = Clock::now();
    for (size_t i = 0; i < prices.size(); ++i) {
        book.add(i, Side::Buy, prices[i], 100);
    }
    auto t1 = Clock::now();
    g_sink = book.num_orders() + book.best_bid()->size;
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

double bench_flat_idmap(const std::vector<int64_t>& prices, uint32_t num_levels) {
    OrderBook book(kMinPrice, num_levels, static_cast<Idx>(prices.size()));
    std::unordered_map<uint64_t, Idx> ids;
    ids.reserve(prices.size());
    auto t0 = Clock::now();
    for (size_t i = 0; i < prices.size(); ++i) {
        ids.emplace(i, book.add(i, Side::Buy, prices[i], 100));
    }
    auto t1 = Clock::now();
    g_sink = ids.size() + book.level(Side::Buy, prices[0]).count;
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

} // namespace

int main(int argc, char** argv) {
    size_t   n          = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
    uint32_t num_levels = argc > 2 ? static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10)) : 1000;
    int      runs       = argc > 3 ? std::atoi(argv[3]) : 5;

    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int64_t> dist(kMinPrice, kMinPrice + num_levels - 1);
    std::vector<int64_t> prices(n);
    for (auto& p : prices) p = dist(rng);

    std::printf("orders=%zu levels=%u runs=%d\n", n, num_levels, runs);
    std::printf("sizeof(Node)=%zu  sizeof(Level)=%zu  sizeof(std::list<Order>)=%zu\n",
                sizeof(Node), sizeof(Level), sizeof(std::list<Order>));

    double best[4] = {1e30, 1e30, 1e30, 1e30};
    const char* names[4] = {"A map+list   ", "S selph      ", "C flat+idmap ", "B flat       "};
    for (int r = 0; r < runs; ++r) {
        double t[4] = {bench_map(prices), bench_selph(prices, num_levels),
                       bench_flat_idmap(prices, num_levels), bench_flat(prices, num_levels)};
        std::printf("  run %d:", r);
        for (int k = 0; k < 4; ++k) {
            std::printf("  %c %6.2f", names[k][0], t[k] / n);
            best[k] = std::min(best[k], t[k]);
        }
        std::printf("  (ns/insert)\n");
    }

    std::printf("best of %d (ns/insert):\n", runs);
    for (int k = 0; k < 4; ++k)
        std::printf("  %s %7.2f   (%.1fx vs B)\n", names[k], best[k] / n, best[k] / best[3]);
}
