// Minimal sanity tests (no framework). Build in Debug so asserts are on.
#include "order_book.hpp"
#include <cassert>
#include <cstdio>

int main() {
    // Prices 23000..23999 ticks ($230.00..$239.99 at 1c), room for 4 orders.
    OrderBook ob(23000, 1000, 4);

    Idx a = ob.add(1, Side::Buy, 23005, 100);
    Idx b = ob.add(2, Side::Buy, 23005, 200);
    Idx c = ob.add(3, Side::Buy, 23005, 400);
    assert(a != NIL && b != NIL && c != NIL);

    [[maybe_unused]] const Level& L = ob.level(Side::Buy, 23005);
    assert(L.count == 3 && L.total_qty == 700);
    assert(L.head == a && L.tail == c);         // FIFO order a, b, c
    assert(ob.node(a).next == b && ob.node(b).next == c);

    // Cancel from the middle
    ob.cancel(b);
    assert(L.count == 2 && L.total_qty == 500);
    assert(ob.node(a).next == c && ob.node(c).prev == a);

    // Cancel head, then tail -> empty level
    ob.cancel(a);
    assert(L.head == c && L.tail == c);
    ob.cancel(c);
    assert(L.head == NIL && L.tail == NIL && L.count == 0);

    // Out-of-range price rejected
    assert(ob.add(9, Side::Sell, 22999, 1) == NIL);
    assert(ob.add(9, Side::Sell, 24000, 1) == NIL);

    // Pool exhaustion: capacity 4
    Idx h[4];
    for (int i = 0; i < 4; ++i) { h[i] = ob.add(10 + i, Side::Sell, 23100, 1); assert(h[i] != NIL); }
    assert(ob.add(99, Side::Sell, 23100, 1) == NIL);

    // LIFO reuse: freed slot comes back first
    ob.cancel(h[2]);
    assert(ob.add(100, Side::Sell, 23100, 1) == h[2]);

    std::puts("all tests passed");
}
