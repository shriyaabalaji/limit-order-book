// Minimal sanity tests (no framework). Build in Debug so asserts are on.
#include "order_book.hpp"
#include <cassert>
#include <cstdio>

int main() {
    // Prices 23000..23999 ticks ($230.00..$239.99 at 1c),
    // room for 4 live orders, order ids 0..999.
    OrderBook ob(23000, 1000, 4, 1000);

    [[maybe_unused]] Idx a = ob.add(1, Side::Buy, 23005, 100).handle;
    [[maybe_unused]] Idx b = ob.add(2, Side::Buy, 23005, 200).handle;
    [[maybe_unused]] Idx c = ob.add(3, Side::Buy, 23005, 400).handle;
    assert(a != NIL && b != NIL && c != NIL);

    [[maybe_unused]] const Level& L = ob.level(Side::Buy, 23005);
    assert(L.count == 3 && L.total_qty == 700);
    assert(L.head == a && L.tail == c);         // FIFO order a, b, c
    assert(ob.node(a).next == b && ob.node(b).next == c);

    // Duplicate id rejected
    assert(ob.add(2, Side::Buy, 23006, 1).handle == NIL);

    // Cancel from the middle, by order id
    assert(ob.cancel(2));
    assert(L.count == 2 && L.total_qty == 500);
    assert(ob.node(a).next == c && ob.node(c).prev == a);

    // Double cancel and unknown id are safe no-ops
    assert(!ob.cancel(2));
    assert(!ob.cancel(500));
    assert(!ob.cancel(5000)); // out of id range

    // Cancel head, then tail -> empty level
    assert(ob.cancel(1));
    assert(L.head == c && L.tail == c);
    assert(ob.cancel(3));
    assert(L.head == NIL && L.tail == NIL && L.count == 0);

    // Out-of-range price / id rejected
    assert(ob.add(9, Side::Sell, 22999, 1).handle == NIL);
    assert(ob.add(9, Side::Sell, 24000, 1).handle == NIL);
    assert(ob.add(1000, Side::Sell, 23100, 1).handle == NIL);

    // Pool exhaustion: capacity 4
    Idx h[4];
    for (int i = 0; i < 4; ++i) { h[i] = ob.add(10 + i, Side::Sell, 23100, 1).handle; assert(h[i] != NIL); }
    assert(ob.add(99, Side::Sell, 23100, 1).handle == NIL);

    // LIFO reuse: freed slot comes back first. A cancelled id can be reused.
    assert(ob.cancel(12));
    assert(ob.add(100, Side::Sell, 23100, 1).handle == h[2]);
    assert(ob.cancel(100));
    assert(ob.add(12, Side::Sell, 23100, 1).handle == h[2]);

    std::puts("all tests passed");
}
