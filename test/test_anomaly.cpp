#include "anomaly.h"
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

static PriceLevel lvl(Price p, Quantity q) {
    PriceLevel l;
    l.price = p;
    l.total_qty = q;
    l.order_count = 1;
    return l;
}

// A book with a normal, small resting size on each side, so anything big has to
// stand out against something.
static std::vector<PriceLevel> bids(Quantity q = 20) { return {lvl(99.90, q)}; }
static std::vector<PriceLevel> asks(Quantity q = 20) { return {lvl(100.10, q)}; }

static OrderActivity act(OrderActivity::Action action, OrderId id, uint64_t ts,
                         bool applied = true) {
    OrderActivity a;
    a.seq     = id;
    a.ts_us   = ts;
    a.action  = action;
    a.order_id = id;
    a.applied = applied;
    a.bids = bids();
    a.asks = asks();
    return a;
}

static OrderActivity place(OrderId id, OrderSide side, Price price, Quantity qty,
                           uint64_t ts, bool applied = true) {
    OrderActivity a = act(OrderActivity::Action::PLACE, id, ts, applied);
    a.side  = side;
    a.type  = OrderType::LIMIT;
    a.price = price;
    a.qty   = qty;
    a.status = OrderStatus::OPEN;
    return a;
}

static OrderActivity cancel(OrderId id, Quantity qty, uint64_t ts,
                            bool applied = true) {
    OrderActivity a = act(OrderActivity::Action::CANCEL, id, ts, applied);
    a.qty = qty;
    a.status = OrderStatus::CANCELLED;
    return a;
}

// Finds a fired signal by name, or a zero strength.
static double strength_of(const std::vector<AnomalySignal>& s, const char* name) {
    for (const auto& x : s) {
        if (x.name == name) return x.strength;
    }
    return 0.0;
}

static const Alert* find_alert(const SpoofDetector& d, OrderId id) {
    for (const auto& a : d.alerts()) {
        if (a.order_id == id) return &a;
    }
    return nullptr;
}

static const AnomalySignal* find_signal(const std::vector<AnomalySignal>& s,
                                        const char* name) {
    for (const auto& x : s) {
        if (x.name == name) return &x;
    }
    return nullptr;
}

// --- Large-order detection ------------------------------------------------

// The size floor must win over a high z-score. A book where everything is 1 lot
// has a zero-variance baseline and no order is an outlier in any useful sense.
void test_small_orders_never_flag_as_large() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 40; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 1, 1000000 + i * 1000));
        d.observe(cancel(i, 1, 1000500 + i * 1000));
    }
    CHECK(d.orders_seen() == 40);
    CHECK(d.orders_large() == 0);
    CHECK(d.alerts_raised() == 0);
}

// A genuinely outsized order against a tight baseline must be called large.
void test_a_real_outlier_is_large() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 40; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1000000 + i * 1000));
        d.observe(cancel(i, 20, 1050000 + i * 1000));
    }
    d.observe(place(500, OrderSide::SELL, 100.10, 900, 2000000));

    const OrderProfile* p = d.profile(500);
    CHECK(p != nullptr);
    if (p) {
        CHECK(p->large);
        CHECK(p->size_zscore > 3.0);
    }
    CHECK(d.orders_large() == 1);
}

// A cold baseline has no right to call anything an outlier. Three orders is not
// a distribution, and claiming a 3-sigma outlier from it is how a detector ends
// up flagging the first order of every session.
void test_cold_baseline_flags_nothing() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 5; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 10, 1000000 + i * 1000));
        d.observe(cancel(i, 10, 1000500 + i * 1000));
    }
    // The 6th is 50x the others. With the warmup at 10 it must still not be large.
    d.observe(place(99, OrderSide::BUY, 99.90, 500, 2000000));
    const OrderProfile* p = d.profile(99);
    CHECK(p != nullptr);
    if (p) CHECK(!p->large);
    CHECK(d.orders_large() == 0);
}

// --- Order lifetime -------------------------------------------------------

void test_lifetime_is_measured_from_placement_to_cancellation() {
    SpoofDetector d;
    d.observe(place(1, OrderSide::BUY, 99.90, 20, 1'000'000));
    d.observe(cancel(1, 20, 1'500'000));
    const OrderProfile* p = d.profile(1);
    CHECK(p != nullptr);
    if (p) {
        CHECK(p->ended);
        CHECK(p->end_status == OrderStatus::CANCELLED);
        CHECK(p->lifetime_us == 500'000);
        CHECK(p->completed_unfilled());
    }
}

// A clock that steps backwards must not yield a negative lifetime, which would
// otherwise read as an infinitely fast cancellation.
void test_backwards_clock_yields_zero_not_negative() {
    SpoofDetector d;
    d.observe(place(1, OrderSide::BUY, 99.90, 20, 5'000'000));
    d.observe(cancel(1, 20, 1'000'000));
    const OrderProfile* p = d.profile(1);
    CHECK(p != nullptr);
    if (p) CHECK(p->lifetime_us == 0.0);
}

// --- Rapid cancellation ---------------------------------------------------

void test_rapid_cancel_of_a_large_unfilled_order_fires() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 40; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1'000'000 + i * 1000));
        d.observe(cancel(i, 20, 1'050'000 + i * 1000));
    }
    d.observe(place(77, OrderSide::SELL, 100.10, 800, 3'000'000));
    d.observe(cancel(77, 800, 3'200'000));   // 200ms, well under the 2s threshold

    const std::vector<AnomalySignal>& s = d.last_signals();
    CHECK(strength_of(s, "rapid_cancellation") > 0.0);
    CHECK(strength_of(s, "large_order") > 0.0);
    CHECK(d.rapid_cancels() == 1);
}

// A slow cancel is not a rapid cancel, however large the order was.
void test_slow_cancellation_does_not_fire() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 40; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1'000'000 + i * 1000));
        d.observe(cancel(i, 20, 1'050'000 + i * 1000));
    }
    d.observe(place(77, OrderSide::SELL, 100.10, 800, 3'000'000));
    d.observe(cancel(77, 800, 60'000'000));  // 57s

    CHECK(strength_of(d.last_signals(), "rapid_cancellation") == 0.0);
    CHECK(d.rapid_cancels() == 0);
}

// The single most important negative test. A large order withdrawn in 100ms is
// exactly what a market maker does on every single order, thousands of times a
// day. It must not raise an alert on its own, however many rules it trips.
void test_one_large_fast_cancel_never_alerts_on_its_own() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 60; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1'000'000 + i * 1000));
        d.observe(cancel(i, 20, 1'050'000 + i * 1000));
    }
    d.observe(place(77, OrderSide::SELL, 100.10, 900, 3'000'000));
    d.observe(cancel(77, 900, 3'100'000));

    // It is recognised as suspicious...
    CHECK(strength_of(d.last_signals(), "large_order") > 0.0);
    CHECK(strength_of(d.last_signals(), "rapid_cancellation") > 0.0);
    // ...and it is still not an alert, because nothing about it is a pattern.
    CHECK(d.alerts_raised() == 0);
}

// --- Repetition -----------------------------------------------------------

// The actual spoofing shape: the same large sell, same price, over and over.
void test_repeated_large_placement_raises_one_alert() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 40; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1'000'000 + i * 1000));
        d.observe(cancel(i, 20, 1'050'000 + i * 1000));
    }
    // Six identical large, fast, unfilled sells at one price.
    for (uint64_t k = 0; k < 6; ++k) {
        const uint64_t ts = 3'000'000 + k * 5'000'000;
        d.observe(place(200 + k, OrderSide::SELL, 100.10, 900, ts));
        d.observe(cancel(200 + k, 900, ts + 300'000));
    }

    CHECK(d.alerts_raised() >= 1);
    const Alert* a = find_alert(d, 205);
    CHECK(a != nullptr);
    if (a) {
        CHECK(a->pattern);
        CHECK(a->score >= d.config().alert_score);
        CHECK(a->related_orders.size() >= 3);
        CHECK(strength_of(a->signals, "repeated_placement") > 0.0);
    }
}

// The same trick on both sides at the same price is two patterns, not one. If
// the clusters merged, the third order on each side would reach the repetition
// threshold using the *other* side's two occurrences, and the reported count
// would read six.
void test_opposite_sides_do_not_share_a_cluster() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 40; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1'000'000 + i * 1000));
        d.observe(cancel(i, 20, 1'050'000 + i * 1000));
    }
    for (uint64_t k = 0; k < 3; ++k) {
        const uint64_t ts = 3'000'000 + k * 5'000'000;
        d.observe(place(300 + k, OrderSide::SELL, 100.10, 900, ts));
        d.observe(cancel(300 + k, 900, ts + 300'000));
    }
    for (uint64_t k = 0; k < 3; ++k) {
        const uint64_t ts = 4'000'000 + k * 5'000'000;
        d.observe(place(400 + k, OrderSide::BUY, 100.10, 900, ts));
        d.observe(cancel(400 + k, 900, ts + 300'000));
    }

    // Three of each is exactly the threshold, so repetition fires -- but it
    // fires on the count of its own side alone.
    const AnomalySignal* r = find_signal(d.last_signals(), "repeated_placement");
    CHECK(r != nullptr);
    if (r) {
        CHECK(r->detail.find("3 large BUY") != std::string::npos);
        CHECK(r->detail.find("6 large") == std::string::npos);
    }
}

// Repetition far apart in time is not repetition. The cluster has to expire.
void test_repetition_outside_the_window_does_not_cluster() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 40; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1'000'000 + i * 1000));
        d.observe(cancel(i, 20, 1'050'000 + i * 1000));
    }
    for (uint64_t k = 0; k < 6; ++k) {
        // 10 minutes apart: the 60s cluster window expires every time.
        const uint64_t ts = 3'000'000 + k * 600'000'000;
        d.observe(place(500 + k, OrderSide::SELL, 100.10, 900, ts));
        d.observe(cancel(500 + k, 900, ts + 300'000));
    }
    CHECK(d.alerts_raised() == 0);
}

// --- Price manipulation ---------------------------------------------------

// A large sell that goes up, the price falls while it is up, then it is pulled.
// The price moved the way the placer wanted, which is the part that is not
// explainable as liquidity provision.
void test_price_moving_the_placers_way_is_a_signal() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 40; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1'000'000 + i * 1000));
        d.observe(cancel(i, 20, 1'050'000 + i * 1000));
    }
    d.observe(place(77, OrderSide::SELL, 100.30, 900, 3'000'000));

    // The mid is the average of the touches, so moving one side moves it by
    // half as much. With the ask held at 100.10, a bid at 99.80 puts the mid at
    // 99.95: 5 ticks down, in the sell's favour.
    OrderActivity c = cancel(77, 900, 4'000'000);
    c.bids = {lvl(99.80, 20)};
    c.asks = {lvl(100.10, 20)};
    d.observe(c);

    const OrderProfile* p = d.profile(77);
    CHECK(p != nullptr);
    if (p) {
        // Stored as a price, not a tick count: 5 ticks of a 0.01 tick.
        CHECK(p->mid_move_favor > 0.049);
        CHECK(p->mid_move_favor < 0.051);
    }
    CHECK(strength_of(d.last_signals(), "price_manipulation") > 0.0);
}

// The sign of "favourable" depends on the side. A large buy wants the price UP,
// so the identical price move must read as zero or negative for it. Getting
// this backwards would flag every seller in the market as a spoofer.
void test_favourable_move_is_signed_by_side() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 40; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1'000'000 + i * 1000));
        d.observe(cancel(i, 20, 1'050'000 + i * 1000));
    }
    // A large BUY goes up, and the price FALLS: against it, not for it.
    d.observe(place(78, OrderSide::BUY, 99.70, 900, 3'000'000));
    OrderActivity c = cancel(78, 900, 4'000'000);
    c.bids = {lvl(99.80, 20)};
    c.asks = {lvl(100.10, 20)};
    d.observe(c);

    const OrderProfile* p = d.profile(78);
    CHECK(p != nullptr);
    if (p) {
        // Same 5-tick drop, for a buy this time: against the order's interest.
        CHECK(p->mid_move_favor < 0.0);
    }
    CHECK(strength_of(d.last_signals(), "price_manipulation") == 0.0);
}

// A large order parked well away from the touch and pulled is the layering
// footprint. One at the touch that gets hit is just trading.
void test_layering_away_from_the_touch_fires() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 40; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1'000'000 + i * 1000));
        d.observe(cancel(i, 20, 1'050'000 + i * 1000));
    }
    // 30 ticks above a 100.00 mid.
    d.observe(place(77, OrderSide::SELL, 100.30, 900, 3'000'000));
    d.observe(cancel(77, 900, 3'300'000));

    const OrderProfile* p = d.profile(77);
    CHECK(p != nullptr);
    if (p) {
        CHECK(p->ticks_from_mid > 25.0);
        CHECK(!p->crossed_touch);   // it never could have traded
    }
    CHECK(strength_of(d.last_signals(), "layering") > 0.0);
}

// An order resting on top of the touch could have been filled at any moment.
// It was not, and it was withdrawn. That choice is worth recording.
void test_crossed_touch_is_recorded() {
    SpoofDetector d;
    OrderActivity p1 = place(1, OrderSide::SELL, 99.90, 500, 1'000'000);
    d.observe(p1);
    const OrderProfile* p = d.profile(1);
    CHECK(p != nullptr);
    if (p) CHECK(p->crossed_touch);
}

// --- Cancellation to placement ratio --------------------------------------

void test_cancel_to_placement_ratio() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 9; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1'000'000 + i * 1000));
    }
    for (uint64_t i = 0; i < 9; ++i) {
        d.observe(cancel(i, 20, 2'000'000 + i * 1000));
    }
    CHECK(std::fabs(d.cancel_to_placement_ratio() - 1.0) < 1e-9);

    d.observe(place(50, OrderSide::BUY, 99.90, 20, 3'000'000));
    CHECK(std::fabs(d.cancel_to_placement_ratio() - 0.9) < 1e-9);
}

// A market maker runs near 0.95 all day, which saturates the ratio context. It
// must still be impossible for that context to manufacture an alert: every
// order here is unremarkable, so the whole run stays quiet.
void test_context_cannot_manufacture_an_alert() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 100; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1'000'000 + i * 1000));
        d.observe(cancel(i, 20, 1'050'000 + i * 1000));
    }
    CHECK(d.cancel_to_placement_ratio() > 0.95);
    // The context fires, at full strength, on the most benign flow imaginable.
    const AnomalySignal* cr = find_signal(d.last_signals(), "cancel_ratio");
    CHECK(cr != nullptr);
    if (cr) {
        CHECK(cr->context);
        CHECK(cr->strength > 0.9);
    }
    // ...and it produced nothing, because no order-level rule fired.
    CHECK(d.alerts_raised() == 0);
    CHECK(d.orders_large() == 0);
}

void test_rates_are_per_second() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 10; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1'000'000 + i * 100'000));
        d.observe(cancel(i, 20, 1'050'000 + i * 100'000));
    }
    // 10 placements spread over 900ms.
    CHECK(std::fabs(d.placements_per_second() - 10.0 / 0.9) < 1e-6);
    CHECK(std::fabs(d.cancellations_per_second() - 10.0 / 0.9) < 1e-6);
}

// --- Fills are not spoofing ------------------------------------------------

// A large order that trades is a large order doing its job. The cancellation
// rules must be structurally unable to fire on it.
void test_a_large_order_that_fills_is_not_a_spoof() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 40; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1'000'000 + i * 1000));
        d.observe(cancel(i, 20, 1'050'000 + i * 1000));
    }
    d.observe(place(77, OrderSide::SELL, 100.10, 900, 3'000'000));

    // A market buy lifts it in full.
    OrderActivity m = act(OrderActivity::Action::PLACE, 78, 3'100'000);
    m.type = OrderType::MARKET;
    m.qty  = 900;
    m.side = OrderSide::BUY;
    m.fills.push_back({77, 78, 900, 100.10, OrderSide::BUY});
    d.observe(m);

    const OrderProfile* p = d.profile(77);
    CHECK(p != nullptr);
    if (p) {
        CHECK(p->end_status == OrderStatus::FILLED);
        CHECK(!p->completed_unfilled());
    }
    CHECK(strength_of(d.last_signals(), "rapid_cancellation") == 0.0);
    CHECK(strength_of(d.last_signals(), "layering") == 0.0);
    CHECK(d.alerts_raised() == 0);
}

// An order that filled on arrival must still have its profile closed. Missed
// cleanup here leaks a profile per instant fill, for the life of the process.
void test_an_order_filled_on_arrival_is_closed() {
    SpoofDetector d;
    OrderActivity a = place(1, OrderSide::BUY, 100.10, 50, 1'000'000);
    a.fills.push_back({2, 1, 50, 100.10, OrderSide::BUY});
    d.observe(a);

    const OrderProfile* p = d.profile(1);
    CHECK(p != nullptr);
    if (p) {
        CHECK(p->ended);
        CHECK(p->end_status == OrderStatus::FILLED);
        CHECK(p->filled_qty == 50);
    }
}

// --- Refused operations ---------------------------------------------------

// A refused order never existed. There is nothing to judge, and judging it
// anyway would let a rejected spam pattern look like a trading pattern.
void test_refused_operations_are_never_tracked() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 10; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1'000'000 + i * 1000, false));
        d.observe(cancel(i, 20, 1'050'000 + i * 1000, false));
    }
    CHECK(d.orders_seen() == 0);
    CHECK(d.orders_large() == 0);
    CHECK(d.alerts_raised() == 0);
    CHECK(d.profile(1) == nullptr);
}

// A modify is neither a placement nor a cancellation and must not be scored as
// one. Spoofing is measured in whole orders, not in edits to them.
void test_modifies_are_ignored() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 40; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1'000'000 + i * 1000));
        d.observe(cancel(i, 20, 1'050'000 + i * 1000));
    }
    d.observe(place(77, OrderSide::SELL, 100.10, 900, 3'000'000));

    OrderActivity m = act(OrderActivity::Action::MODIFY, 77, 3'100'000);
    m.qty = 10;
    d.observe(m);

    const OrderProfile* p = d.profile(77);
    CHECK(p != nullptr);
    if (p) {
        CHECK(!p->ended);           // still working
        CHECK(p->lifetime_us == 0.0);
    }
    CHECK(d.alerts_raised() == 0);
}

// --- Scoring --------------------------------------------------------------

// The multiplicity cap: the score may not exceed 100/min_signals on one signal.
void test_a_single_signal_is_capped_below_the_alert_threshold() {
    AnomalyConfig cfg;
    cfg.min_signals    = 3.0;
    cfg.alert_score    = 60.0;
    cfg.require_pattern = false;
    SpoofDetector d(cfg);

    // One large, slow, unfilled, far-from-touch order: large_order plus
    // layering, but no repetition and no price movement.
    for (uint64_t i = 0; i < 40; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1'000'000 + i * 1000));
        d.observe(cancel(i, 20, 1'050'000 + i * 1000));
    }
    d.observe(place(77, OrderSide::SELL, 100.30, 900, 3'000'000));
    d.observe(cancel(77, 900, 3'500'000));

    CHECK(d.last_signals().size() >= 2);
    // With 2 signals against a requirement of 3, the cap keeps it quiet.
    CHECK(d.alerts_raised() == 0);
}

// require_pattern is the second guard: repetition or price movement must be
// present, so a pile of independent small suspicions still stays quiet.
void test_require_pattern_blocks_alerts_without_repetition_or_impact() {
    AnomalyConfig cfg;
    cfg.require_pattern = true;
    cfg.alert_score     = 10.0;   // so only the pattern guard can be binding
    cfg.min_signals     = 1.0;
    SpoofDetector d(cfg);

    for (uint64_t i = 0; i < 40; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1'000'000 + i * 1000));
        d.observe(cancel(i, 20, 1'050'000 + i * 1000));
    }
    // Large, fast, unfilled, far from touch: 3 signals, no pattern.
    d.observe(place(77, OrderSide::SELL, 100.30, 900, 3'000'000));
    d.observe(cancel(77, 900, 3'300'000));

    CHECK(d.last_signals().size() >= 3);
    CHECK(strength_of(d.last_signals(), "repeated_placement") == 0.0);
    CHECK(strength_of(d.last_signals(), "price_manipulation") == 0.0);
    CHECK(d.alerts_raised() == 0);
}

// Every signal in an alert must be individually explainable, with a weight
// that is exactly strength * its rule weight and a non-empty detail string.
void test_every_alert_signal_is_explainable() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 40; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1'000'000 + i * 1000));
        d.observe(cancel(i, 20, 1'050'000 + i * 1000));
    }
    for (uint64_t k = 0; k < 6; ++k) {
        const uint64_t ts = 3'000'000 + k * 5'000'000;
        d.observe(place(600 + k, OrderSide::SELL, 100.30, 900, ts));
        OrderActivity c = cancel(600 + k, 900, ts + 300'000);
        c.bids = {lvl(99.50, 20)};
        c.asks = {lvl(100.10, 20)};
        d.observe(c);
    }
    CHECK(!d.alerts().empty());
    for (const auto& a : d.alerts()) {
        CHECK(a.score > 0.0 && a.score <= 100.0);
        CHECK(!a.signals.empty());
        CHECK(!a.summary.empty());
        for (const auto& s : a.signals) {
            CHECK(s.strength > 0.0 && s.strength <= 1.0);
            CHECK(s.weight > 0.0);
            CHECK(!s.name.empty());
            CHECK(!s.detail.empty());
        }
    }
}

// --- Report ---------------------------------------------------------------

void test_report_is_well_formed() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 40; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1'000'000 + i * 1000));
        d.observe(cancel(i, 20, 1'050'000 + i * 1000));
    }
    for (uint64_t k = 0; k < 6; ++k) {
        const uint64_t ts = 3'000'000 + k * 5'000'000;
        d.observe(place(700 + k, OrderSide::SELL, 100.30, 900, ts));
        d.observe(cancel(700 + k, 900, ts + 300'000));
    }
    const std::string r = d.report();
    CHECK(r.find("orders tracked") != std::string::npos);
    CHECK(r.find("cancel / place") != std::string::npos);
    CHECK(r.find("lifetime") != std::string::npos);
    CHECK(r.find("alerts") != std::string::npos);
    CHECK(r.find("repeated_placement") != std::string::npos);
}

// --- Pipeline integration -------------------------------------------------

static const char* kSpoofFeed =
    "ts=1000000 ADD SELL LIMIT 100.20 20 1\n"
    "ts=1050000 CANCEL 1\n"
    "ts=2000000 ADD BUY LIMIT 99.90 20 2\n"
    "ts=2050000 CANCEL 2\n"
    "ts=3000000 ADD BUY LIMIT 99.90 20 3\n"
    "ts=3050000 CANCEL 3\n"
    "ts=4000000 ADD BUY LIMIT 99.90 20 4\n"
    "ts=4050000 CANCEL 4\n"
    "ts=5000000 ADD BUY LIMIT 99.90 20 5\n"
    "ts=5050000 CANCEL 5\n"
    "ts=6000000 ADD BUY LIMIT 99.90 20 6\n"
    "ts=6050000 CANCEL 6\n"
    "ts=7000000 ADD BUY LIMIT 99.90 20 7\n"
    "ts=7050000 CANCEL 7\n"
    "ts=8000000 ADD BUY LIMIT 99.90 20 8\n"
    "ts=8050000 CANCEL 8\n"
    "ts=9000000 ADD BUY LIMIT 99.90 20 9\n"
    "ts=9050000 CANCEL 9\n"
    "ts=10000000 ADD BUY LIMIT 99.90 20 10\n"
    "ts=10050000 CANCEL 10\n"
    "ts=11000000 ADD BUY LIMIT 99.90 20 11\n"
    "ts=11050000 CANCEL 11\n"
    "ts=12000000 ADD BUY LIMIT 99.90 20 12\n"
    "ts=12050000 CANCEL 12\n"
    // Six large, fast, unfilled sells at one price, above the touch.
    "ts=13000000 ADD SELL LIMIT 100.30 900 100\n"
    "ts=13100000 CANCEL 100\n"
    "ts=13500000 ADD SELL LIMIT 100.30 900 101\n"
    "ts=13600000 CANCEL 101\n"
    "ts=14000000 ADD SELL LIMIT 100.30 900 102\n"
    "ts=14100000 CANCEL 102\n"
    "ts=14500000 ADD SELL LIMIT 100.30 900 103\n"
    "ts=14600000 CANCEL 103\n"
    "ts=15000000 ADD SELL LIMIT 100.30 900 104\n"
    "ts=15100000 CANCEL 104\n"
    "ts=15500000 ADD SELL LIMIT 100.30 900 105\n"
    "ts=15600000 CANCEL 105\n";

void test_pipeline_drives_the_detector() {
    OrderBook book("TEST", 0.01);
    Pipeline pipe(book);
    SpoofDetector det;
    pipe.set_detector(&det);

    pipe.submit_text(kSpoofFeed);

    CHECK(det.orders_seen() == 18);
    CHECK(det.orders_large() == 6);
    CHECK(det.rapid_cancels() == 6);
    CHECK(det.alerts_raised() >= 1);

    // The engine is untouched by any of it.
    CHECK(book.validate());
}

// Refused lines must not reach the detector as tradable activity. Only the one
// accepted add in this feed is a real order; the other four never existed.
void test_pipeline_refusals_do_not_reach_the_detector() {
    OrderBook book("TEST", 0.01);
    Pipeline pipe(book);
    SpoofDetector det;
    pipe.set_detector(&det);

    pipe.submit_text(
        "ts=1000000 ADD BUY LIMIT 100.00 10 1\n"     // accepted: one real order
        "ts=1100000 CANCEL 999\n"                    // unknown order
        "ts=1200000 MODIFY 998 5\n"                  // unknown order
        "ts=1300000 BOGUS VERB 1 2 3\n"              // unknown verb
        "ts=1400000 not a feed line at all\n"         // unknown verb
    );

    CHECK(det.orders_seen() == 1);
    CHECK(det.profile(1) != nullptr);
    // None of the refused lines produced an order the detector could judge.
    CHECK(det.orders_closed() == 0);
    CHECK(det.alerts_raised() == 0);
}

// The parser must name the real unknown verb, not the timestamp in front of it.
void test_unknown_verb_behind_a_timestamp_is_named_correctly() {
    OrderBook book("TEST", 0.01);
    Pipeline pipe(book);
    auto out = pipe.submit_text("ts=1300000 BOGUS VERB 1 2 3\n");
    CHECK(out.size() == 1);
    if (out.size() == 1) {
        CHECK(out[0].error.find("BOGUS") != std::string::npos);
        CHECK(out[0].error.find("ts=") == std::string::npos);
    }
}

// The same feed through the engine must produce the same book, with the
// detector attached or not. Detection must not be able to change the outcome.
void test_detector_does_not_change_the_book() {
    OrderBook a("TEST", 0.01);
    Pipeline pa(a);
    pa.submit_text(kSpoofFeed);

    OrderBook b("TEST", 0.01);
    Pipeline pb(b);
    SpoofDetector det;
    pb.set_detector(&det);
    pb.submit_text(kSpoofFeed);

    CHECK(a.validate() && b.validate());
    CHECK(a.sequence() == b.sequence());
    CHECK(a.depth(10).bids.size() == b.depth(10).bids.size());
}

// The placement context must be the market as it was *before* the order
// arrived. Measured after, an aggressive order is judged against a book its own
// size already moved, so its apparent price impact is structurally zero and
// every large order that genuinely lifted the market looks inert.
void test_placement_context_precedes_the_order() {
    SpoofDetector d;
    for (uint64_t i = 0; i < 40; ++i) {
        d.observe(place(i, OrderSide::BUY, 99.90, 20, 1'000'000 + i * 1000));
        d.observe(cancel(i, 20, 1'050'000 + i * 1000));
    }

    // A large sell that takes the top bid out, taking the mid down 5 ticks.
    OrderActivity a = place(90, OrderSide::SELL, 99.90, 400, 3'000'000);
    a.prev_mid = 100.00;            // the mid before it arrives
    a.bids     = {lvl(99.80, 20)};  // the book after: the top bid is gone
    a.asks     = {lvl(100.10, 20)};
    a.fills.push_back({77, 90, 400, 99.90, OrderSide::SELL});
    d.observe(a);

    const OrderProfile* p = d.profile(90);
    CHECK(p != nullptr);
    if (p) {
        if (p->mid_at_place) CHECK(*p->mid_at_place == 100.00);
        // Measured against the pre-op mid, the market moved 5 ticks down, and
        // for a sell that is 5 ticks its way.
        CHECK(p->mid_move_favor > 0.049);
    }
    CHECK(strength_of(d.last_signals(), "price_manipulation") > 0.0);
}

int main() {
    test_small_orders_never_flag_as_large();
    test_a_real_outlier_is_large();
    test_cold_baseline_flags_nothing();

    test_lifetime_is_measured_from_placement_to_cancellation();
    test_backwards_clock_yields_zero_not_negative();

    test_rapid_cancel_of_a_large_unfilled_order_fires();
    test_slow_cancellation_does_not_fire();
    test_one_large_fast_cancel_never_alerts_on_its_own();

    test_repeated_large_placement_raises_one_alert();
    test_opposite_sides_do_not_share_a_cluster();
    test_repetition_outside_the_window_does_not_cluster();

    test_price_moving_the_placers_way_is_a_signal();
    test_favourable_move_is_signed_by_side();
    test_layering_away_from_the_touch_fires();
    test_crossed_touch_is_recorded();

    test_cancel_to_placement_ratio();
    test_context_cannot_manufacture_an_alert();
    test_rates_are_per_second();

    test_a_large_order_that_fills_is_not_a_spoof();
    test_an_order_filled_on_arrival_is_closed();

    test_refused_operations_are_never_tracked();
    test_modifies_are_ignored();

    test_a_single_signal_is_capped_below_the_alert_threshold();
    test_require_pattern_blocks_alerts_without_repetition_or_impact();
    test_every_alert_signal_is_explainable();
    test_placement_context_precedes_the_order();

    test_report_is_well_formed();

    test_pipeline_drives_the_detector();
    test_pipeline_refusals_do_not_reach_the_detector();
    test_unknown_verb_behind_a_timestamp_is_named_correctly();
    test_detector_does_not_change_the_book();

    if (failures) {
        std::cerr << failures << " test(s) FAILED\n";
        return 1;
    }
    std::cout << "All anomaly detection tests passed\n";
    return 0;
}
