// Matching engine tests (no framework). Build in Debug so asserts are on.
#include "order_book.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

namespace {

struct Recorder {
    std::vector<Trade> trades;
    void operator()(const Trade& t) { trades.push_back(t); }
};

OrderBook make_book() { return OrderBook(10000, 1000, 64, 1000); } // prices 10000..10999

void test_no_cross() {
    OrderBook ob = make_book();
    Recorder r;
    ob.add(1, Side::Sell, 10050, 100, r);
    ob.add(2, Side::Buy, 10049, 100, r); // 1 tick below the ask: no trade
    assert(r.trades.empty());
    assert(ob.best_bid() == 10049 && ob.best_ask() == 10050);
}

void test_full_fill_at_resting_price() {
    OrderBook ob = make_book();
    Recorder r;
    ob.add(1, Side::Sell, 10050, 100, r);
    [[maybe_unused]] AddResult res = ob.add(2, Side::Buy, 10060, 100, r); // willing to pay more

    assert(res.accepted && res.filled == 100 && res.handle == NIL); // nothing rests
    assert(r.trades.size() == 1);
    [[maybe_unused]] const Trade& t = r.trades[0];
    assert(t.taker_id == 2 && t.maker_id == 1 && t.qty == 100);
    assert(t.price_ticks == 10050);  // resting order's price, not 10060
    assert(!ob.is_live(1) && !ob.is_live(2));
    assert(!ob.best_bid() && !ob.best_ask()); // book empty
}

void test_partial_fill_taker_rests() {
    OrderBook ob = make_book();
    Recorder r;
    ob.add(1, Side::Sell, 10050, 30, r);
    [[maybe_unused]] AddResult res = ob.add(2, Side::Buy, 10050, 100, r);

    assert(res.filled == 30 && res.handle != NIL);
    assert(ob.level(Side::Buy, 10050).total_qty == 70); // remainder rests as a bid
    assert(ob.best_bid() == 10050 && !ob.best_ask());
}

void test_partial_fill_maker_keeps_priority() {
    OrderBook ob = make_book();
    Recorder r;
    ob.add(1, Side::Sell, 10050, 100, r);
    ob.add(2, Side::Sell, 10050, 100, r);
    ob.add(3, Side::Buy, 10050, 40, r);  // partially fills order 1

    assert(r.trades.size() == 1 && r.trades[0].maker_id == 1 && r.trades[0].qty == 40);
    assert(ob.node(ob.level(Side::Sell, 10050).head).order_id == 1); // 1 still first in line
    assert(ob.node(ob.level(Side::Sell, 10050).head).qty == 60);

    r.trades.clear();
    ob.add(4, Side::Buy, 10050, 100, r); // finishes 1 (60), then 40 of 2
    assert(r.trades.size() == 2);
    assert(r.trades[0].maker_id == 1 && r.trades[0].qty == 60);
    assert(r.trades[1].maker_id == 2 && r.trades[1].qty == 40);
}

void test_fifo_within_level() {
    OrderBook ob = make_book();
    Recorder r;
    ob.add(1, Side::Buy, 10040, 10, r);
    ob.add(2, Side::Buy, 10040, 10, r);
    ob.add(3, Side::Buy, 10040, 10, r);
    ob.add(4, Side::Sell, 10040, 25, r);

    assert(r.trades.size() == 3);
    assert(r.trades[0].maker_id == 1 && r.trades[1].maker_id == 2 && r.trades[2].maker_id == 3);
    assert(r.trades[2].qty == 5);
}

void test_sweep_multiple_levels() {
    OrderBook ob = make_book();
    Recorder r;
    ob.add(1, Side::Sell, 10050, 10, r);
    ob.add(2, Side::Sell, 10051, 10, r);
    ob.add(3, Side::Sell, 10055, 10, r);
    ob.add(4, Side::Sell, 10060, 10, r);
    [[maybe_unused]] AddResult res = ob.add(5, Side::Buy, 10055, 100, r); // sweeps 3 levels, stops at 10060

    assert(res.filled == 30);
    assert(r.trades.size() == 3);
    assert(r.trades[0].price_ticks == 10050);
    assert(r.trades[1].price_ticks == 10051);
    assert(r.trades[2].price_ticks == 10055);
    assert(ob.best_ask() == 10060);           // next level becomes best
    assert(ob.best_bid() == 10055);           // remainder 70 rests at its limit
}

void test_sell_side_matching() {
    OrderBook ob = make_book();
    Recorder r;
    ob.add(1, Side::Buy, 10045, 10, r);
    ob.add(2, Side::Buy, 10040, 10, r);
    ob.add(3, Side::Sell, 10040, 15, r); // hits highest bid first

    assert(r.trades.size() == 2);
    assert(r.trades[0].maker_id == 1 && r.trades[0].price_ticks == 10045);
    assert(r.trades[1].maker_id == 2 && r.trades[1].price_ticks == 10040 && r.trades[1].qty == 5);
    assert(ob.best_bid() == 10040 && !ob.best_ask());
}

void test_cancel_updates_best() {
    OrderBook ob = make_book();
    ob.add(1, Side::Buy, 10040, 10);
    ob.add(2, Side::Buy, 10045, 10);
    ob.add(3, Side::Sell, 10060, 10);
    ob.add(4, Side::Sell, 10070, 10);
    assert(ob.best_bid() == 10045 && ob.best_ask() == 10060);

    ob.cancel(2);
    ob.cancel(3);
    assert(ob.best_bid() == 10040 && ob.best_ask() == 10070);
    ob.cancel(1);
    ob.cancel(4);
    assert(!ob.best_bid() && !ob.best_ask());
}

void test_filled_node_recycled() {
    OrderBook ob(10000, 1000, 2, 1000); // pool of only 2 nodes
    ob.add(1, Side::Sell, 10050, 10);
    ob.add(2, Side::Sell, 10050, 10);
    ob.add(3, Side::Buy, 10050, 20);    // fills both, frees both nodes
    assert(ob.add(4, Side::Sell, 10050, 10).handle != NIL);
    assert(ob.add(5, Side::Sell, 10050, 10).handle != NIL);
}

void test_pool_full_remainder_reported() {
    OrderBook ob(10000, 1000, 1, 1000); // pool of 1 node
    Recorder r;
    ob.add(1, Side::Sell, 10050, 30, r);     // uses the only node
    ob.add(2, Side::Sell, 10060, 10, r);     // pool full, no match -> all cancelled
    [[maybe_unused]] AddResult res = ob.add(3, Side::Buy, 10050, 100, r); // fills 30, frees node, rests 70
    assert(res.filled == 30 && res.handle != NIL && res.cancelled == 0);

    [[maybe_unused]] AddResult full = ob.add(4, Side::Sell, 10070, 10, r); // pool full again
    assert(full.accepted && full.filled == 0 && full.handle == NIL);
    assert(full.cancelled == 10 && full.reason == CancelReason::PoolFull);
    assert(!ob.is_live(4));
}

void test_pool_full_after_partial_fill() {
    OrderBook ob(10000, 1000, 2, 1000);
    ob.add(1, Side::Sell, 10050, 30);
    ob.add(2, Side::Buy, 10040, 10);          // pool now full (2 nodes)
    // Buy 100: fills 30 vs order 1 (frees a node), rests 70 in that node.
    // Then a sell that partially fills and can't rest:
    ob.add(3, Side::Buy, 10050, 100);
    [[maybe_unused]] AddResult res = ob.add(4, Side::Sell, 10050, 100); // fills 70 vs order 3, frees a node, rests 30
    assert(res.filled == 70 && res.handle != NIL);
    [[maybe_unused]] AddResult res2 = ob.add(5, Side::Buy, 10045, 5);   // no match, pool full
    assert(res2.cancelled == 5 && res2.reason == CancelReason::PoolFull);
    // filled + cancelled + rested always equals qty
    assert(res.filled + res.cancelled + ob.node(res.handle).qty == 100);
}

void test_self_trade_cancels_newest() {
    OrderBook ob = make_book();
    Recorder r;
    ob.add(1, Side::Sell, 10050, 10, /*owner=*/7, r);
    ob.add(2, Side::Sell, 10050, 10, /*owner=*/9, r);
    [[maybe_unused]] AddResult res = ob.add(3, Side::Buy, 10050, 20, /*owner=*/7, r);

    // Owner 7's own resting sell is first in line -> stop immediately.
    assert(r.trades.empty());
    assert(res.filled == 0 && res.cancelled == 20 && res.reason == CancelReason::SelfTrade);
    assert(res.handle == NIL && !ob.is_live(3));
    assert(ob.is_live(1) && ob.is_live(2)); // resting orders untouched
}

void test_self_trade_after_other_fills() {
    OrderBook ob = make_book();
    Recorder r;
    ob.add(1, Side::Sell, 10050, 10, /*owner=*/9, r);
    ob.add(2, Side::Sell, 10051, 10, /*owner=*/7, r);
    [[maybe_unused]] AddResult res = ob.add(3, Side::Buy, 10055, 30, /*owner=*/7, r);

    // Trades with owner 9 stand; stops at owner 7's order; rest cancelled.
    assert(r.trades.size() == 1 && r.trades[0].maker_id == 1);
    assert(res.filled == 10 && res.cancelled == 20 && res.reason == CancelReason::SelfTrade);
    assert(ob.is_live(2) && ob.best_ask() == 10051);
}

void test_no_owner_means_no_stp() {
    OrderBook ob = make_book();
    Recorder r;
    ob.add(1, Side::Sell, 10050, 10, r);  // kNoOwner
    ob.add(2, Side::Buy, 10050, 10, r);   // kNoOwner: trades normally
    assert(r.trades.size() == 1);
}

} // namespace

int main() {
    test_no_cross();
    test_full_fill_at_resting_price();
    test_partial_fill_taker_rests();
    test_partial_fill_maker_keeps_priority();
    test_fifo_within_level();
    test_sweep_multiple_levels();
    test_sell_side_matching();
    test_cancel_updates_best();
    test_filled_node_recycled();
    test_pool_full_remainder_reported();
    test_pool_full_after_partial_fill();
    test_self_trade_cancels_newest();
    test_self_trade_after_other_fills();
    test_no_owner_means_no_stp();
    std::puts("all matching tests passed");
}
