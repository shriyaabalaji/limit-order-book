// Perp layer tests (no framework). Build in Debug so asserts are on.
#include "perp.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <random>

namespace {

constexpr Owner A = 1, B = 2, C = 3;

// Prices $90.00..$109.99 in cents. 10% initial margin.
// B and C get $1M each; A gets `a_deposit` (cents).
PerpConfig small_cfg() {
    PerpConfig c;
    c.min_price_ticks = 9000; c.num_levels = 2000; c.max_orders = 1024;
    c.max_order_id = 100000; c.max_accounts = 16;
    return c;
}

PerpExchange make_ex(int64_t a_deposit = 100'000'000) {
    PerpExchange ex(small_cfg());
    ex.deposit(A, a_deposit);
    ex.deposit(B, 100'000'000);
    ex.deposit(C, 100'000'000);
    return ex;
}

void test_mark_is_mid_and_sticky() {
    PerpExchange ex = make_ex();
    assert(!ex.mark());                       // no book yet
    ex.place(C, Side::Buy, 9990, 5);
    assert(!ex.mark());                       // only one side
    [[maybe_unused]] uint64_t ask = ex.place(C, Side::Sell, 10010, 5).id;
    assert(ex.mark() == 10000);               // (9990 + 10010) / 2
    ex.cancel(C, ask);
    assert(ex.mark() == 10000);               // ask side empty: keep last mark
}

void test_open_and_unrealized_is_zero_sum() {
    PerpExchange ex = make_ex();
    ex.place(B, Side::Sell, 10000, 10);       // B rests an ask
    ex.place(A, Side::Buy, 10000, 10);        // A lifts it: A long 10, B short 10
    assert(ex.account(A).size == 10 && ex.account(B).size == -10);
    assert(ex.entry_price(A) == 10000.0 && ex.entry_price(B) == 10000.0);

    ex.place(C, Side::Buy, 10100, 1);         // C quotes around 102.00
    ex.place(C, Side::Sell, 10300, 1);
    assert(ex.mark() == 10200);
    assert(ex.unrealized(A) == 2000);         // (102 - 100) * 10 = $20.00
    assert(ex.unrealized(B) == -2000);
    assert(ex.unrealized(C) == 0);            // C has no position
}

void test_weighted_average_entry() {
    PerpExchange ex = make_ex();
    ex.place(B, Side::Sell, 10000, 1);
    ex.place(B, Side::Sell, 10050, 2);
    ex.place(A, Side::Buy, 10050, 3);         // sweeps both levels
    assert(ex.account(A).size == 3 && ex.account(A).cost == 10000 + 2 * 10050);
    assert(std::abs(ex.entry_price(A) - 10033.33) < 0.01); // $100.33
}

void test_partial_close_realizes_pnl() {
    PerpExchange ex = make_ex(100000);       // A has $1,000
    ex.place(B, Side::Sell, 10050, 3);
    ex.place(B, Side::Sell, 10150, 1);
    ex.place(A, Side::Buy, 10150, 4);         // A long 4, cost 3*10050 + 10150 = 40300
    assert(ex.account(A).cost == 40300);

    ex.place(C, Side::Buy, 10300, 2);         // C bids 103.00
    ex.place(A, Side::Sell, 10300, 2);        // A sells 2 into it
    // closed half the position: cost removed 20150, proceeds 20600 -> +$4.50
    assert(ex.account(A).realized == 450);
    assert(ex.account(A).collateral == 100000 + 450);
    assert(ex.account(A).size == 2 && ex.account(A).cost == 20150);
    assert(ex.entry_price(A) == 10075.0);     // entry unchanged by reducing
}

void test_flip_long_to_short() {
    PerpExchange ex = make_ex();
    ex.place(B, Side::Sell, 10000, 2);
    ex.place(A, Side::Buy, 10000, 2);         // A long 2 @ 100
    ex.place(C, Side::Buy, 10100, 5);
    ex.place(A, Side::Sell, 10100, 5);        // sells 5: closes 2, opens short 3
    assert(ex.account(A).realized == 200);    // (101 - 100) * 2
    assert(ex.account(A).size == -3 && ex.account(A).cost == -30300);
    assert(ex.entry_price(A) == 10100.0);
}

void test_cancel_only_by_owner() {
    PerpExchange ex = make_ex();
    [[maybe_unused]] uint64_t id = ex.place(A, Side::Buy, 9900, 1).id;
    assert(!ex.cancel(B, id));
    assert(ex.cancel(A, id));
}

void test_initial_margin_limit() {
    PerpExchange ex = make_ex(100000);       // A has $1,000
    ex.place(C, Side::Buy, 9990, 1);
    ex.place(C, Side::Sell, 10010, 1);       // mark = $100.00
    // 10% of 100 contracts * $100 = $1,000: exactly enough
    assert(ex.place(A, Side::Buy, 9900, 101).margin_rejected);
    [[maybe_unused]] PlaceResult r = ex.place(A, Side::Buy, 9900, 100);
    assert(!r.margin_rejected && r.book.handle != NIL);  // rests, unfilled
    assert(ex.account(A).open_buy == 100);
    // the resting 100 already uses all the margin, even though nothing filled
    assert(ex.place(A, Side::Buy, 9900, 1).margin_rejected);
    // a sell doesn't raise worst case (max(|0+100|, |0-1|) = 100): allowed
    assert(!ex.place(A, Side::Sell, 10200, 1).margin_rejected);
    // cancelling frees the margin
    ex.cancel(A, r.id);
    assert(ex.account(A).open_buy == 0);
    assert(!ex.place(A, Side::Buy, 9900, 50).margin_rejected);
}

void test_reducing_always_allowed() {
    PerpExchange ex = make_ex(100000);       // A has $1,000
    ex.place(B, Side::Sell, 10000, 100);
    ex.place(A, Side::Buy, 10000, 100);      // long 100 @ $100 (10x)
    ex.place(C, Side::Buy, 9490, 1);
    ex.place(C, Side::Sell, 9510, 1);        // mark falls to $95.00
    assert(ex.equity(A) == 50000);           // $1,000 - $500
    assert(ex.place(A, Side::Buy, 9000, 1).margin_rejected);     // can't add
    assert(!ex.place(A, Side::Sell, 9600, 50).margin_rejected);  // can reduce
    assert(ex.account(A).open_sell == 50);
    // selling 250 more would flip to short 200: raises worst case, rejected
    assert(ex.place(A, Side::Sell, 9600, 250).margin_rejected);
}

void test_open_qty_tracks_maker_fills() {
    PerpExchange ex = make_ex();
    ex.place(A, Side::Buy, 10000, 10);
    assert(ex.account(A).open_buy == 10);
    ex.place(B, Side::Sell, 10000, 4);       // fills 4 of A's resting bid
    assert(ex.account(A).open_buy == 6 && ex.account(A).size == 4);
    assert(ex.account(B).open_sell == 0);    // B's order never rested
}

// Book for liquidation tests: C quotes ask $95.00 x1, a top bid of $94.80 x1
// (the one we'll pull), then the given deeper bids. Returns the top bid's id.
uint64_t quote_book(PerpExchange& ex, std::initializer_list<std::pair<int64_t, uint32_t>> deep_bids) {
    ex.place(C, Side::Sell, 9500, 1);
    uint64_t top = ex.place(C, Side::Buy, 9480, 1).id;
    for (auto [px, q] : deep_bids) ex.place(C, Side::Buy, px, q);
    return top;
}

void test_liquidation_closes_into_book() {
    PerpExchange ex = make_ex(100000);       // A has $1,000
    ex.place(B, Side::Sell, 10000, 100);
    ex.place(A, Side::Buy, 10000, 100);      // long 100 @ $100, 10x
    // liq price: 1,000 + 100*(P - 100) = 0.05 * 100 * P  ->  P = 900,000 / 95 = $94.74
    assert(std::abs(ex.liquidation_price(A) - 900000.0 / 95.0) < 1e-6);

    uint64_t top = quote_book(ex, {{9400, 100}});
    assert(ex.mark() == 9490 && ex.account(A).size == 100); // $94.90: still above $94.74
    ex.take_liquidations();

    std::vector<Trade> tape;
    ex.cancel(C, top, [&](const Trade& t) { tape.push_back(t); }); // mark -> (9400+9500)/2 = $94.50
    assert(ex.account(A).size == 0);
    assert(ex.account(A).collateral == 100000 - 600 * 100); // sold 100 @ $94: lost $600
    assert(ex.account(A).liquidations == 1);
    auto liq = ex.take_liquidations();
    assert(liq.size() == 1 && liq[0].who == A && liq[0].size == 100 && liq[0].closed == 100);
    assert(liq[0].bad_debt == 0);
    assert(tape.size() == 1 && tape[0].price_ticks == 9400 && tape[0].qty == 100);
}

void test_liquidation_bad_debt() {
    PerpExchange ex = make_ex(105000);       // A has $1,050
    ex.place(B, Side::Sell, 10500, 100);
    ex.place(A, Side::Buy, 10500, 100);      // long 100 @ $105, 10x
    ex.place(C, Side::Sell, 10000, 1);
    uint64_t top = ex.place(C, Side::Buy, 9990, 1).id;
    ex.place(C, Side::Buy, 9000, 200);       // the only real liquidity: $90
    assert(ex.account(A).size == 100);       // mark $99.95, liq price ~$99.47

    ex.cancel(C, top);                       // mark -> $95.00: liquidate into the $90 bid
    // lost $15 x 100 = $1,500 with only $1,050: $450 of bad debt
    assert(ex.account(A).size == 0 && ex.account(A).collateral == 0);
    assert(ex.bad_debt() == 45000);
    assert(ex.take_liquidations().back().bad_debt == 45000);
    [[maybe_unused]] int64_t eq = 0;
    for (Owner o = 1; o < 16; ++o) eq += ex.equity(o);
    assert(eq == ex.total_deposits() + ex.bad_debt()); // B was paid $450 that A never had
}

void test_cascade() {
    PerpExchange ex = make_ex(100000);       // A: $1,000 on 100 contracts (10x)
    constexpr Owner D = 4;
    ex.deposit(D, 120000);                   // D: $1,200 on 100 contracts (~8.3x)
    ex.place(B, Side::Sell, 10000, 200);
    ex.place(A, Side::Buy, 10000, 100);
    ex.place(D, Side::Buy, 10000, 100);
    // liq prices: A $94.74, D 880,000/95 = $92.63
    uint64_t top = quote_book(ex, {{9300, 100}, {9000, 100}});
    ex.take_liquidations();

    // One cancel: mark $94.50 -> A dumps into $93 -> best bid $90 ->
    // mark (9000+9500)/2 = $92.50 -> D is under too -> D dumps into $90.
    ex.cancel(C, top);
    auto liq = ex.take_liquidations();
    assert(liq.size() == 2 && liq[0].who == A && liq[1].who == D);
    assert(ex.account(A).collateral == 100000 - 700 * 100);  // sold @ $93
    assert(ex.account(D).collateral == 120000 - 1000 * 100); // sold @ $90
    assert(ex.mark() == 9250);               // no bids left: mark sticks
}

void test_funding_zero_sum_and_direction() {
    PerpExchange ex = make_ex();
    ex.place(B, Side::Sell, 10100, 10);
    ex.place(A, Side::Buy, 10100, 10);       // A long 10, B short 10
    ex.place(C, Side::Buy, 10090, 1);
    ex.place(C, Side::Sell, 10110, 1);       // mark $101.00
    [[maybe_unused]] int64_t a0 = ex.account(A).collateral, b0 = ex.account(B).collateral;
    // index $100.80: perp is $0.20 rich -> longs pay 20 cents per contract
    [[maybe_unused]] int64_t f = ex.apply_funding(10080);
    assert(f == 20);
    assert(ex.account(A).collateral == a0 - 200 && ex.account(A).funding == -200);
    assert(ex.account(B).collateral == b0 + 200);
    // index far below: capped at 0.5% of index ($0.50)
    assert(ex.apply_funding(10000) == 50);
    // index above mark: shorts pay longs
    assert(ex.apply_funding(10150) == -50);
}

// Random flow: total equity must always equal total deposits, because
// every fill moves value from one trader to another and creates none.
void test_random_flow_conserves_money() {
    PerpConfig cfg = small_cfg();
    cfg.max_orders = 4096; cfg.max_order_id = 1'000'000;
    PerpExchange ex(cfg);
    constexpr Owner kTraders = 10;
    [[maybe_unused]] int64_t deposits = 0;
    for (Owner o = 1; o <= kTraders; ++o) {           // traders 1-3 are thinly funded
        int64_t d = o <= 3 ? 30'000 : 1'000'000;
        ex.deposit(o, d);
        deposits += d;
    }

    std::mt19937 rng(42);
    std::vector<uint64_t> live;
    int64_t mid = 10000;
    int rejected = 0, liquidations = 0;
    for (int i = 0; i < 50'000; ++i) {
        Owner who = 1 + rng() % kTraders;
        if (rng() % 4 == 0 && !live.empty()) {
            size_t k = rng() % live.size();
            const Node* n = ex.book().find(live[k]);
            if (n) ex.cancel(n->owner, live[k]);
            live[k] = live.back(); live.pop_back();
            continue;
        }
        mid += static_cast<int64_t>(rng() % 7) - 3;   // drifting fair value
        mid = std::clamp<int64_t>(mid, 9200, 10800);
        Side s = rng() % 2 ? Side::Buy : Side::Sell;
        if (who <= 3) s = who % 2 ? Side::Buy : Side::Sell; // thin traders always pile in one way
        int64_t px = mid + (s == Side::Buy ? -1 : 1) * (static_cast<int64_t>(rng() % 20) - 4);
        PlaceResult r = ex.place(who, s, px, 1 + rng() % 50);
        if (r.book.handle != NIL) live.push_back(r.id);
        if (r.margin_rejected) ++rejected;
        if (i % 500 == 0) ex.apply_funding(mid);   // index = the drifting fair value
        liquidations += static_cast<int>(ex.take_liquidations().size());

        if (i % 1000 == 0) {
            [[maybe_unused]] int64_t total = 0, net_size = 0;
            for (Owner o = 1; o <= kTraders; ++o) { total += ex.equity(o); net_size += ex.account(o).size; }
            assert(net_size == 0);       // every long has a short
            assert(total == deposits + ex.bad_debt()); // only bad debt is "created"
            // open-order counters match what's actually resting in the book
            std::vector<int64_t> buys(kTraders + 1), sells(kTraders + 1);
            for (uint64_t id : live)
                if (const Node* n = ex.book().find(id))
                    (n->side == Side::Buy ? buys : sells)[n->owner] += n->qty;
            for (Owner o = 1; o <= kTraders; ++o)
                assert(ex.account(o).open_buy == buys[o] && ex.account(o).open_sell == sells[o]);
        }
    }
    std::printf("random flow: %d orders rejected for margin, %d liquidations, bad debt $%.2f\n",
                rejected, liquidations, ex.bad_debt() / 100.0);
}

} // namespace

int main() {
    test_mark_is_mid_and_sticky();
    test_open_and_unrealized_is_zero_sum();
    test_weighted_average_entry();
    test_partial_close_realizes_pnl();
    test_flip_long_to_short();
    test_cancel_only_by_owner();
    test_initial_margin_limit();
    test_reducing_always_allowed();
    test_open_qty_tracks_maker_fills();
    test_liquidation_closes_into_book();
    test_liquidation_bad_debt();
    test_cascade();
    test_funding_zero_sum_and_direction();
    test_random_flow_conserves_money();
    std::puts("all perp tests passed");
}
