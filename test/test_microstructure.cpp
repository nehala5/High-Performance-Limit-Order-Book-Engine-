#include "microstructure.h"
#include "pipeline.h"

#include <cmath>
#include <iostream>
#include <sstream>

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << #cond << " @line " << __LINE__ << "\n"; \
        ++failures; \
    } \
} while (0)

static bool near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

static PriceLevel lvl(Price p, Quantity q) {
    PriceLevel l;
    l.price = p;
    l.total_qty = q;
    l.order_count = 1;
    return l;
}

// An observation with an explicit clock, so every rate is exactly checkable.
static Observation obs(uint64_t ts_us, std::vector<PriceLevel> bids,
                       std::vector<PriceLevel> asks) {
    Observation o;
    o.ts_us = ts_us;
    o.bids  = std::move(bids);
    o.asks  = std::move(asks);
    return o;
}

static Observation trade_obs(uint64_t ts_us, Quantity buy, Quantity sell,
                             std::vector<PriceLevel> bids, std::vector<PriceLevel> asks) {
    Observation o = obs(ts_us, std::move(bids), std::move(asks));
    o.buy_qty     = buy;
    o.sell_qty    = sell;
    o.trade_qty   = buy + sell;
    o.trade_count = (buy + sell) > 0 ? 1 : 0;
    o.trade_notional = 0.0;   // filled in by the test where it matters
    return o;
}

// --- Basic quote and depth ------------------------------------------------

void test_mid_and_spread() {
    MicrostructureAnalyzer a;
    a.observe(obs(1000, {lvl(100, 10)}, {lvl(101, 10)}));

    const Metrics& m = a.current();
    CHECK(m.best_bid.has_value() && near(*m.best_bid, 100.0));
    CHECK(m.best_ask.has_value() && near(*m.best_ask, 101.0));
    CHECK(m.mid_price.has_value() && near(*m.mid_price, 100.5));
    CHECK(m.spread.has_value() && near(*m.spread, 1.0));
    CHECK(m.has_mid);

    // A one-sided book has a side but no mid and no spread: dividing a one-sided
    // quote by two is a fiction, not a measurement.
    MicrostructureAnalyzer b;
    b.observe(obs(1000, {lvl(100, 10)}, {}));
    CHECK(b.current().best_bid.has_value());
    CHECK(!b.current().best_ask.has_value());
    CHECK(!b.current().mid_price.has_value());
    CHECK(!b.current().spread.has_value());
    CHECK(!b.current().has_mid);
}

void test_depth_and_imbalance() {
    MicrostructureAnalyzer a;
    a.observe(obs(1000, {lvl(100, 10), lvl(99, 5)}, {lvl(101, 20)}));

    const Metrics& m = a.current();
    CHECK(m.bid_depth == 15);
    CHECK(m.ask_depth == 20);
    CHECK(m.total_depth == 35);
    CHECK(near(m.depth_imbalance, (15.0 - 20.0) / 35.0));
    CHECK(m.depth_imbalance < 0.0);   // more offer than bid
}

void test_imbalance_depth_is_limited() {
    MicrostructureConfig cfg;
    cfg.imbalance_depth = 1;
    MicrostructureAnalyzer a(cfg);
    a.observe(obs(1000, {lvl(100, 10), lvl(99, 1000)}, {lvl(101, 10)}));

    // Only the touch counts when the depth is 1.
    CHECK(a.current().bid_depth == 10);
    CHECK(a.current().ask_depth == 10);
    CHECK(near(a.current().depth_imbalance, 0.0));
}

// The two imbalance measures are not synonyms: nearness weighting moves a book
// with equal size but lopsided placement to a different answer.
void test_liquidity_imbalance_differs_from_depth_imbalance() {
    MicrostructureAnalyzer a;
    // 1 unit at the bid, 9 units nine ticks below it, 10 units at the ask.
    a.observe(obs(1000, {lvl(100, 1), lvl(90, 9)}, {lvl(101, 10)}));

    const Metrics& m = a.current();
    CHECK(m.total_depth == 20);
    CHECK(near(m.depth_imbalance, 0.0));   // unweighted: exactly balanced

    // Weighted by 1/(1+|p-mid|) with mid 100.5, the far bid counts for much
    // less, so the book reads as heavily offer-side.
    CHECK(m.liquidity_imbalance < -0.5);
    CHECK(!near(m.liquidity_imbalance, m.depth_imbalance, 1e-6));
}

void test_empty_book_is_safe() {
    MicrostructureAnalyzer a;
    a.observe(obs(1000, {}, {}));

    const Metrics& m = a.current();
    CHECK(!m.has_mid);
    CHECK(m.total_depth == 0);
    CHECK(near(m.depth_imbalance, 0.0));
    CHECK(near(m.liquidity_imbalance, 0.0));
    CHECK(near(m.vwap, 0.0));
    CHECK(near(m.vpin, 0.0));
    CHECK(near(m.ofi, 0.0));
    CHECK(near(m.order_arrival_rate, 0.0));
    CHECK(near(m.cancel_rate, 0.0));
    CHECK(near(m.trade_intensity, 0.0));
    CHECK(near(m.ewma_vol, 0.0));
    // Nothing divided by zero anywhere: every figure is finite.
    CHECK(std::isfinite(m.realized_vol_annualized));
    CHECK(std::isfinite(m.ewma_vol_annualized));
}

// --- Trade metrics --------------------------------------------------------

void test_volume_vwap_and_buy_sell_split() {
    MicrostructureAnalyzer a;
    // 10 @ 100 and 6 @ 101, all buy-initiated.
    Observation o1 = trade_obs(1000, 10, 0, {lvl(99, 10)}, {lvl(100, 10)});
    o1.trade_notional = 100.0 * 10.0;
    a.observe(o1);

    Observation o2 = trade_obs(2000, 6, 0, {lvl(99, 10)}, {lvl(100, 10)});
    o2.trade_notional = 101.0 * 6.0;
    a.observe(o2);

    const Metrics& m = a.current();
    CHECK(m.trade_count == 2);
    CHECK(m.trade_volume == 16);
    CHECK(m.buy_volume == 16);
    CHECK(m.sell_volume == 0);
    CHECK(near(m.trade_notional, 1000.0 + 606.0));
    CHECK(near(m.vwap, 1606.0 / 16.0));
    CHECK(near(m.avg_trade_size, 8.0));
}

void test_trade_intensity_uses_elapsed_time() {
    MicrostructureAnalyzer a;
    // 4 observations spread over 2 seconds, 2 of them trading.
    a.observe(trade_obs(1000000, 5, 0, {lvl(99, 5)}, {lvl(100, 5)}));
    a.observe(obs(2000000, {lvl(99, 5)}, {lvl(100, 5)}));
    a.observe(trade_obs(2500000, 5, 0, {lvl(99, 5)}, {lvl(100, 5)}));
    a.observe(obs(3000000, {lvl(99, 5)}, {lvl(100, 5)}));

    const Metrics& m = a.current();
    CHECK(near(m.elapsed_seconds, 2.0));
    CHECK(m.trade_count == 2);
    CHECK(near(m.trade_intensity, 1.0));        // 2 trades / 2s
    CHECK(near(m.volume_intensity, 5.0));        // 10 shares / 2s
}

// --- Order flow imbalance -------------------------------------------------

// Cont, Kukanov & Stoikov: size added at an unchanged or better bid, minus size
// removed from an unchanged or worse ask.
void test_ofi_accumulates_buy_pressure() {
    MicrostructureAnalyzer a;
    a.observe(obs(1000, {lvl(100, 10)}, {lvl(101, 10)}));   // seed
    CHECK(near(a.current().ofi, 0.0));   // no previous quote yet

    a.observe(obs(2000, {lvl(100, 15)}, {lvl(101, 10)}));   // +5 at the bid
    CHECK(near(a.current().ofi, 5.0));

    a.observe(obs(3000, {lvl(100, 15)}, {lvl(101, 4)}));    // -6 at the ask
    CHECK(near(a.current().ofi, 11.0));

    a.observe(obs(4000, {lvl(100.5, 20)}, {lvl(101, 4)}));  // bid improves, +20
    CHECK(near(a.current().ofi, 31.0));

    // All three steps were positive, so normalized OFI is steps/mean|step|.
    CHECK(near(a.current().ofi_normalized, 3.0));
}

// OFI measures *order flow*, not trade direction: withdrawing sell liquidity
// removes supply, which is positive. Only a thinner bid reads as negative.
void test_ofi_sign_follows_the_side_withdrawn() {
    MicrostructureAnalyzer ask_shrinks;
    ask_shrinks.observe(obs(1000, {lvl(100, 10)}, {lvl(101, 10)}));
    ask_shrinks.observe(obs(2000, {lvl(100, 10)}, {lvl(101, 4)}));   // -6 at the ask
    CHECK(near(ask_shrinks.current().ofi, 6.0));

    MicrostructureAnalyzer bid_shrinks;
    bid_shrinks.observe(obs(1000, {lvl(100, 10)}, {lvl(101, 10)}));
    bid_shrinks.observe(obs(2000, {lvl(100, 4)}, {lvl(101, 10)}));   // -6 at the bid
    CHECK(near(bid_shrinks.current().ofi, -6.0));
    CHECK(bid_shrinks.current().ofi < 0.0);
    CHECK(ask_shrinks.current().ofi > 0.0);
}

// A quote appearing from nothing is all addition; one that vanishes is all
// removal. Without this the measure would read a one-sided book as no activity.
void test_ofi_handles_one_sided_books() {
    MicrostructureAnalyzer a;
    a.observe(obs(1000, {lvl(100, 10)}, {}));          // bid only
    a.observe(obs(2000, {}, {lvl(101, 10)}));          // bid pulled, ask appears

    // bid -10 (pulled), ask +10 (appeared) => step -20
    CHECK(near(a.current().ofi, -20.0));

    MicrostructureAnalyzer b;
    b.observe(obs(1000, {}, {lvl(101, 10)}));
    b.observe(obs(2000, {lvl(100, 10)}, {}));
    CHECK(near(b.current().ofi, 20.0));
}

// A bid that moves down is a withdrawal at the old price plus an addition at the
// new one; OFI must not credit the new size twice.
void test_ofi_does_not_double_count_a_quote_move() {
    MicrostructureAnalyzer a;
    a.observe(obs(1000, {lvl(100, 10)}, {lvl(101, 5)}));
    a.observe(obs(2000, {lvl(99, 10)}, {lvl(101, 5)}));   // bid steps down

    // Worse bid: no new size counted at 99, and the 10 at 100 is gone.
    CHECK(near(a.current().ofi, -10.0));
}

// --- VPIN -----------------------------------------------------------------

void test_vpin_on_a_fixed_bucket() {
    MicrostructureConfig cfg;
    cfg.vpin_bucket_qty = 10;
    MicrostructureAnalyzer a(cfg);

    auto q = [] { return std::vector<PriceLevel>{lvl(100, 10)}; };

    a.observe(trade_obs(1000, 10, 0, q(), q()));   // bucket 1: +10
    CHECK(a.current().vpin_buckets == 1);
    CHECK(near(a.current().vpin, 1.0));

    a.observe(trade_obs(2000, 5, 5, q(), q()));    // bucket 2: 0
    CHECK(a.current().vpin_buckets == 2);
    CHECK(near(a.current().vpin, 10.0 / 2 / 10.0));

    a.observe(trade_obs(3000, 0, 10, q(), q()));   // bucket 3: -10
    CHECK(a.current().vpin_buckets == 3);
    CHECK(near(a.current().vpin, 20.0 / 3 / 10.0));
}

// A single observation larger than the bucket size must close more than one.
void test_vpin_drains_multiple_buckets() {
    MicrostructureConfig cfg;
    cfg.vpin_bucket_qty = 10;
    MicrostructureAnalyzer a(cfg);
    auto q = [] { return std::vector<PriceLevel>{lvl(100, 10)}; };

    a.observe(trade_obs(1000, 40, 0, q(), q()));
    CHECK(a.current().vpin_buckets == 4);
    CHECK(near(a.current().vpin, 1.0));
}

// A partially filled bucket is not yet a measurement.
void test_vpin_is_zero_until_a_bucket_closes() {
    MicrostructureConfig cfg;
    cfg.vpin_bucket_qty = 10;
    MicrostructureAnalyzer a(cfg);
    auto q = [] { return std::vector<PriceLevel>{lvl(100, 10)}; };

    a.observe(trade_obs(1000, 3, 0, q(), q()));
    CHECK(a.current().vpin_buckets == 0);
    CHECK(near(a.current().vpin, 0.0));

    a.observe(trade_obs(2000, 3, 0, q(), q()));   // 6 of 10 accumulated
    CHECK(a.current().vpin_buckets == 0);
    CHECK(near(a.current().vpin, 0.0));

    a.observe(trade_obs(3000, 4, 0, q(), q()));   // closes at +10
    CHECK(a.current().vpin_buckets == 1);
    CHECK(near(a.current().vpin, 1.0));
}

// The volume clock must actually close a bucket per bucket_size of volume. If
// the target is re-derived from a running mean it can shrink below the volume
// already accumulated, and every share then closes a bucket.
void test_vpin_bucket_count_tracks_volume() {
    MicrostructureAnalyzer a;   // bucket size derived from the first trade: 20
    auto q = [] { return std::vector<PriceLevel>{lvl(100, 10)}; };

    // First trade sizes the bucket at 20 shares.
    a.observe(trade_obs(1000, 20, 0, q(), q()));
    CHECK(a.current().vpin_buckets == 1);

    // Later trades are deliberately small and lopsided, which is exactly what
    // would shrink a re-derived target.
    for (int i = 0; i < 10; ++i) {
        Observation o = trade_obs(2000 + i * 1000, 1, 1, q(), q());
        a.observe(o);
    }
    CHECK(a.current().trade_volume == 20 + 20);
    // 40 shares of volume at 20 per bucket: exactly two buckets in total.
    CHECK(a.current().vpin_buckets == 2);

    // 25 more shares takes it to 65, which is three full buckets.
    Observation o = trade_obs(20000, 25, 0, q(), q());
    a.observe(o);
    CHECK(a.current().vpin_buckets == 3);
    CHECK(a.current().trade_volume == 65);
}

// --- Volatility -----------------------------------------------------------

void test_realized_volatility_sums_squared_log_returns() {
    MicrostructureAnalyzer a;
    a.observe(obs(1000, {lvl(99, 10)}, {lvl(101, 10)}));    // mid 100
    a.observe(obs(2000, {lvl(100, 10)}, {lvl(102, 10)}));    // mid 101
    a.observe(obs(3000, {lvl(99.5, 10)}, {lvl(101.5, 10)}));// mid 100.5

    const double r1 = std::log(101.0 / 100.0);
    const double r2 = std::log(100.5 / 101.0);
    CHECK(a.current().return_count == 2);
    CHECK(near(a.current().realized_vol, r1 * r1 + r2 * r2));
    CHECK(a.current().ewma_vol > 0.0);
}

void test_realized_vol_window() {
    MicrostructureConfig cfg;
    cfg.rv_window = 1;
    MicrostructureAnalyzer a(cfg);

    a.observe(obs(1000, {lvl(99, 10)}, {lvl(101, 10)}));
    a.observe(obs(2000, {lvl(100, 10)}, {lvl(102, 10)}));
    a.observe(obs(3000, {lvl(99.5, 10)}, {lvl(101.5, 10)}));

    const double r2 = std::log(100.5 / 101.0);
    // Only the most recent return is inside the window.
    CHECK(near(a.current().realized_vol, r2 * r2));
}

void test_ewma_follows_its_recursion() {
    MicrostructureConfig cfg;
    cfg.ewma_lambda = 0.94;
    MicrostructureAnalyzer a(cfg);

    a.observe(obs(1000, {lvl(99, 10)}, {lvl(101, 10)}));
    a.observe(obs(2000, {lvl(100, 10)}, {lvl(102, 10)}));
    a.observe(obs(3000, {lvl(99.5, 10)}, {lvl(101.5, 10)}));

    const double r1 = std::log(101.0 / 100.0);
    const double r2 = std::log(100.5 / 101.0);
    const double v1 = r1 * r1;
    const double v2 = 0.94 * v1 + 0.06 * r2 * r2;

    CHECK(near(a.current().ewma_variance, v2));
    CHECK(near(a.current().ewma_vol, std::sqrt(v2)));
    // Seeded from the first return, so the second observation already has a
    // variance rather than starting from zero.
    MicrostructureAnalyzer b(cfg);
    b.observe(obs(1000, {lvl(99, 10)}, {lvl(101, 10)}));
    b.observe(obs(2000, {lvl(100, 10)}, {lvl(102, 10)}));
    CHECK(near(b.current().ewma_variance, v1));
}

void test_annualization_scales_by_periods_per_year() {
    MicrostructureConfig cfg;
    cfg.periods_per_year = 252.0;
    MicrostructureAnalyzer a(cfg);
    a.observe(obs(1000, {lvl(99, 10)}, {lvl(101, 10)}));
    a.observe(obs(2000, {lvl(100, 10)}, {lvl(102, 10)}));

    const double r1 = std::log(101.0 / 100.0);
    CHECK(near(a.current().realized_vol_annualized, std::sqrt(r1 * r1 * 252.0)));
    CHECK(near(a.current().ewma_vol_annualized, std::sqrt(r1 * r1) * std::sqrt(252.0)));
}

// A book that never changes has no return, and therefore no volatility.
void test_flat_book_has_no_volatility() {
    MicrostructureAnalyzer a;
    a.observe(obs(1000, {lvl(99, 10)}, {lvl(101, 10)}));
    a.observe(obs(2000, {lvl(99, 10)}, {lvl(101, 10)}));
    a.observe(obs(3000, {lvl(99, 10)}, {lvl(101, 10)}));

    CHECK(a.current().return_count == 2);
    CHECK(near(a.current().realized_vol, 0.0));
    CHECK(near(a.current().ewma_vol, 0.0));
    CHECK(near(a.current().realized_vol_annualized, 0.0));
}

// --- Arrivals and cancellations ------------------------------------------

void test_order_arrival_and_cancel_rates() {
    MicrostructureAnalyzer a;

    // 4 orders and 1 cancel over 1 second.
    for (int i = 0; i < 4; ++i) {
        Observation o = obs(1000 + i * 250000, {lvl(100, 10)}, {lvl(101, 10)});
        o.is_add = true;
        o.applied = true;
        o.order_qty = 10;
        a.observe(o);
    }
    Observation c = obs(1001000, {lvl(100, 10)}, {lvl(101, 10)});
    c.is_cancel = true;
    c.applied = true;
    c.leaves_qty = 10;
    a.observe(c);

    const Metrics& m = a.current();
    CHECK(m.orders == 4);
    CHECK(m.cancels == 1);
    CHECK(near(m.elapsed_seconds, 1.0));
    CHECK(near(m.order_arrival_rate, 4.0));      // orders per second
    CHECK(near(m.volume_arrival_rate, 40.0));    // shares per second
    CHECK(near(m.cancel_rate, 1.0 / 5.0));       // cancels / (orders + cancels)
    CHECK(near(m.cancel_rate_by_volume, 10.0 / 40.0));
}

// A refused operation is not an arrival and not a cancellation.
void test_refused_operations_do_not_count_as_activity() {
    MicrostructureAnalyzer a;
    Observation o = obs(1000, {lvl(100, 10)}, {lvl(101, 10)});
    o.is_add = true;
    o.applied = true;
    o.order_qty = 10;
    a.observe(o);

    Observation bad = obs(2000, {lvl(100, 10)}, {lvl(101, 10)});
    bad.is_cancel = true;
    bad.applied = false;      // refused
    a.observe(bad);

    CHECK(a.current().orders == 1);
    CHECK(a.current().cancels == 0);
    CHECK(near(a.current().cancel_rate, 0.0));
    CHECK(near(a.current().cancel_rate_by_volume, 0.0));
}

void test_history_records_every_observation() {
    MicrostructureAnalyzer a;
    a.observe(obs(1000, {lvl(100, 10)}, {lvl(101, 10)}));
    a.observe(obs(2000, {lvl(100, 15)}, {lvl(101, 10)}));
    a.observe(obs(3000, {lvl(100, 15)}, {lvl(101, 10)}));

    CHECK(a.history().size() == 3);
    CHECK(near(a.history()[0].ofi, 0.0));
    CHECK(near(a.history()[1].ofi, 5.0));
    CHECK(near(a.history()[2].ofi, 5.0));
    CHECK(near(a.history()[0].mid_price.value(), 100.5));
}

// --- Integration with the pipeline ----------------------------------------

void test_pipeline_drives_the_analyzer() {
    OrderBook book("T", 0.01);
    Pipeline pipe(book);
    MicrostructureAnalyzer ana;
    pipe.set_analyzer(&ana);

    // ts= is nanoseconds on the wire, and the pipeline converts to microseconds
    // for the analyzer. These are one second apart.
    auto outcomes = pipe.submit_text(
        "ADD SELL LIMIT 100.00 10 1 ts=1700000000000000000\n"   // t+0s
        "ADD BUY  LIMIT  99.00  5 2 ts=1700000001000000000\n"   // t+1s
        "CANCEL 1            ts=1700000002000000000\n"          // t+2s
        "BAD LINE HERE       ts=1700000003000000000\n"          // malformed
        "ADD BUY  LIMIT 105.00 5 1 ts=1700000004000000000\n");  // duplicate id

    CHECK(outcomes.size() == 5);

    // One observation per operation that changed the book: the two adds and the
    // cancel. The malformed line and the refused add are not book changes.
    CHECK(ana.current().observations == 3);
    CHECK(ana.history().size() == 3);

    const Metrics& m = ana.current();
    CHECK(m.orders == 2);
    CHECK(m.cancels == 1);
    CHECK(near(m.cancel_rate, 1.0 / 3.0));
    CHECK(m.best_bid.has_value() && near(*m.best_bid, 99.0));
    CHECK(!m.best_ask.has_value());
    CHECK(!m.has_mid);
    CHECK(near(m.elapsed_seconds, 2.0));
    CHECK(near(m.order_arrival_rate, 1.0));

    // The cancel withdrew 10 against 15 submitted.
    CHECK(near(m.cancel_rate_by_volume, 10.0 / 15.0));
}

void test_pipeline_trades_reach_the_analyzer() {
    OrderBook book("T", 0.01);
    Pipeline pipe(book);
    MicrostructureAnalyzer ana;
    pipe.set_analyzer(&ana);

    pipe.submit_text(
        "ADD SELL LIMIT 100.00 100 1 ts=1700000000000000000\n"
        "ADD SELL LIMIT 101.00 100 2 ts=1700000001000000000\n"
        "ADD BUY  LIMIT 101.00 150 3 ts=1700000002000000000\n");

    const Metrics& m = ana.current();
    // The buy sweeps 100 of order 1 at 100, then 50 of order 2 at 101.
    CHECK(m.trade_count == 2);
    CHECK(m.trade_volume == 150);
    CHECK(m.buy_volume == 150);
    CHECK(near(m.trade_notional, 100.0 * 100.0 + 101.0 * 50.0));
    CHECK(near(m.vwap, (100.0 * 100.0 + 101.0 * 50.0) / 150.0));
    CHECK(near(m.elapsed_seconds, 2.0));
    CHECK(near(m.trade_intensity, 1.0));
    CHECK(m.orders == 3);
    CHECK(m.observations == 3);
}

// No analyzer attached must be free, and must not change the book's behaviour.
void test_pipeline_works_without_an_analyzer() {
    OrderBook book("T");
    Pipeline pipe(book);
    CHECK(pipe.analyzer() == nullptr);
    auto o = pipe.submit_line("ADD SELL LIMIT 100 10 1");
    CHECK(o.applied);
    CHECK(near(*book.best_ask(), 100.0));
}

int main() {
    test_mid_and_spread();
    test_depth_and_imbalance();
    test_imbalance_depth_is_limited();
    test_liquidity_imbalance_differs_from_depth_imbalance();
    test_empty_book_is_safe();

    test_volume_vwap_and_buy_sell_split();
    test_trade_intensity_uses_elapsed_time();

    test_ofi_accumulates_buy_pressure();
    test_ofi_sign_follows_the_side_withdrawn();
    test_ofi_handles_one_sided_books();
    test_ofi_does_not_double_count_a_quote_move();

    test_vpin_on_a_fixed_bucket();
    test_vpin_drains_multiple_buckets();
    test_vpin_is_zero_until_a_bucket_closes();
    test_vpin_bucket_count_tracks_volume();

    test_realized_volatility_sums_squared_log_returns();
    test_realized_vol_window();
    test_ewma_follows_its_recursion();
    test_annualization_scales_by_periods_per_year();
    test_flat_book_has_no_volatility();

    test_order_arrival_and_cancel_rates();
    test_refused_operations_do_not_count_as_activity();
    test_history_records_every_observation();

    test_pipeline_drives_the_analyzer();
    test_pipeline_trades_reach_the_analyzer();
    test_pipeline_works_without_an_analyzer();

    if (failures == 0) std::cout << "All tests passed\n";
    else               std::cout << failures << " test(s) FAILED\n";
    return failures == 0 ? 0 : 1;
}
