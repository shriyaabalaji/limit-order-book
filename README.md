# limit-order-book

I'm trying to get more into understanding how markets work, and first I wanted to dive into limit order books. This is my journey building one and what I take away from it.

Written in C++20. Reading Harris, *Trading and Exchanges*, alongside.

**Try it:** [Order Book Playground](https://shriyaabalaji.github.io/limit-order-book/), the same C++ engine compiled to WebAssembly so you can place orders and watch them match in the browser.

## What's here so far

- `include/order_book.hpp` - the order book
  - flat array of price levels, indexed by price tick
  - each level is a FIFO queue (doubly linked list) of orders, so the oldest order at a price gets filled first
  - all order nodes are allocated up front in a pool, with a free list to hand them out and take them back
- `tests/` - sanity tests for the book and the matching engine
- `web/` - the browser playground (`web/build.sh` compiles the engine with Emscripten)
- `bench/` - insert benchmark comparing my book against `std::map` + `std::list` and a version of the design from WK Selph's "How to Build a Fast Limit Order Book"

Also has matching (price-time priority, partial fills), best bid/ask tracking, cancel by order id, and self-trade prevention.

## Build and run

```
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
./build-release/bench_insert            # defaults: 1M orders, 1000 price levels, 5 runs
./build-release/bench_insert 1000000 100000
```

Tests:

```
cmake -S . -B build && cmake --build build && ./build/test_order_book
```

## Results

1M inserts, 1000 price levels, best of 5, on an Apple M-series laptop:

| design | ns per insert |
|---|---|
| `std::map` + `std::list` | 68 |
| Selph-style (tree + hash maps + heap allocated orders) | 42 |
| my book + `std::unordered_map` for order ids | 22 |
| my book | 4 |

## Takeaways

- The design comes down to how efficient adding, filling, and cancelling orders are, and that depends on the data structures you use.
- My version is faster because it assumes a bounded, dense price range and a max number of orders, so it doesn't have to search or allocate memory on each insert. The other designs don't assume anything, so they pay for that every time.
- The best implementation changes based on the asset. Crypto has a much larger, sparser price range, while typical stocks are bounded and more dense.
- Caching matters for how long things take, not just big O.

More notes in `learning.txt`.
