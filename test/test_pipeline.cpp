#include "pipeline.h"
#include "feed.h"

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

#define CHECK_VALID(book) do { \
    std::string err; \
    if (!(book).validate(&err)) { \
        std::cerr << "FAIL: invariant violated: " << err << " @line " << __LINE__ << "\n"; \
        ++failures; \
    } \
} while (0)

static bool near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

static bool same_depth(const L2Snapshot& a, const L2Snapshot& b) {
    if (a.bids.size() != b.bids.size()) return false;
    if (a.asks.size() != b.asks.size()) return false;
    for (size_t i = 0; i < a.bids.size(); ++i) {
        if (a.bids[i].price != b.bids[i].price) return false;
        if (a.bids[i].total_qty != b.bids[i].total_qty) return false;
        if (a.bids[i].order_count != b.bids[i].order_count) return false;
    }
    for (size_t i = 0; i < a.asks.size(); ++i) {
        if (a.asks[i].price != b.asks[i].price) return false;
        if (a.asks[i].total_qty != b.asks[i].total_qty) return false;
        if (a.asks[i].order_count != b.asks[i].order_count) return false;
    }
    return a.best_bid == b.best_bid && a.best_ask == b.best_ask
        && a.mid_price == b.mid_price && a.spread == b.spread;
}

// --- Parser ---------------------------------------------------------------

void test_parser_reads_each_verb() {
    auto add = FeedParser::parse("ADD BUY LIMIT 100.50 25 7");
    CHECK(add.ok);
    CHECK(add.event.type == FeedEventType::ADD);
    CHECK(add.event.side == OrderSide::BUY);
    CHECK(add.event.order_type == OrderType::LIMIT);
    CHECK(near(add.event.price, 100.50));
    CHECK(add.event.qty == 25);
    CHECK(add.event.order_id == 7);

    auto mk = FeedParser::parse("ADD SELL MARKET 5 8");
    CHECK(mk.ok);
    CHECK(mk.event.order_type == OrderType::MARKET);
    CHECK(mk.event.side == OrderSide::SELL);
    CHECK(mk.event.qty == 5);
    CHECK(mk.event.order_id == 8);

    auto cx = FeedParser::parse("CANCEL 42");
    CHECK(cx.ok);
    CHECK(cx.event.type == FeedEventType::CANCEL);
    CHECK(cx.event.order_id == 42);

    auto md = FeedParser::parse("MODIFY 42 15");
    CHECK(md.ok);
    CHECK(md.event.type == FeedEventType::MODIFY);
    CHECK(md.event.new_qty == 15);
    CHECK(!md.event.has_new_price);

    auto md2 = FeedParser::parse("MODIFY 42 15 99.25");
    CHECK(md2.ok);
    CHECK(md2.event.has_new_price);
    CHECK(near(md2.event.new_price, 99.25));

    auto tp = FeedParser::parse("TRADE 100.25 3 11 12");
    CHECK(tp.ok);
    CHECK(tp.event.type == FeedEventType::TRADE_PRINT);
    CHECK(tp.event.buy_order_id == 11);
    CHECK(tp.event.sell_order_id == 12);
    CHECK(tp.event.qty == 3);
}

// The venue timestamp may sit anywhere on the line and must not be mistaken
// for a positional field.
void test_parser_extracts_timestamp() {
    auto a = FeedParser::parse("ADD BUY LIMIT 10 1 5 ts=123456789");
    CHECK(a.ok);
    CHECK(a.event.exchange_ts == 123456789ULL);
    CHECK(a.event.qty == 1);
    CHECK(a.event.order_id == 5);

    auto b = FeedParser::parse("ts=99 CANCEL 5");
    CHECK(b.ok);
    CHECK(b.event.exchange_ts == 99ULL);
    CHECK(b.event.type == FeedEventType::CANCEL);
    CHECK(b.event.order_id == 5);

    CHECK(!FeedParser::parse("CANCEL 5 ts=abc").ok);
    CHECK(!FeedParser::parse("CANCEL 5 ts=").ok);
}

void test_parser_rejects_malformed() {
    const char* bad[] = {
        "ADD BUY LIMIT 100",             // too few
        "ADD BUY LIMIT 100 5",           // no id
        "ADD BUY LIMIT 100 5 1 9",       // too many
        "ADD BUY MARKET 5",              // no id
        "ADD BUY MARKET 5 8 9",          // price is not a market field
        "ADD MAYBE LIMIT 100 5 1",       // bad side
        "ADD BUY STOP 100 5 1",          // bad type
        "ADD BUY LIMIT abc 5 1",         // price not a number
        "ADD BUY LIMIT 100 5.5 1",       // qty not a whole number
        "ADD BUY LIMIT 100 5 xyz",       // id not a whole number
        "CANCEL",                        // no id
        "CANCEL 1 2",                    // too many
        "MODIFY 1",                      // no qty
        "MODIFY 1 5 6 7",                // too many
        "MODIFY 1 abc",                  // qty not a number
        "TRADE 100 5 1",                 // missing side
    };
    for (const char* line : bad) {
        FeedParseResult r = FeedParser::parse(line);
        if (r.ok) std::cerr << "FAIL: should not parse: " << line << " @line " << __LINE__ << "\n";
        CHECK(!r.ok);
        CHECK(!r.error.empty());
    }
}

// An inline note is part of the line's provenance, not part of its fields.
void test_parser_strips_inline_comments() {
    auto r = FeedParser::parse("ADD BUY LIMIT 100 5 1 # rest my order");
    CHECK(r.ok);
    CHECK(r.event.type == FeedEventType::ADD);
    CHECK(r.event.side == OrderSide::BUY);
    CHECK(near(r.event.price, 100.0));
    CHECK(r.event.qty == 5);
    CHECK(r.event.order_id == 1);
    // The raw line is still kept whole, for the audit trail.
    CHECK(r.event.raw == "ADD BUY LIMIT 100 5 1 # rest my order");

    auto c = FeedParser::parse("CANCEL 5#5");
    CHECK(c.ok);
    CHECK(c.event.order_id == 5);

    // A line that is only a comment is nothing to do.
    auto only = FeedParser::parse("   # just a note");
    CHECK(only.ok);
    CHECK(only.event.type == FeedEventType::NONE);
}

// Blank and comment lines are not errors; they simply do nothing. An
// unrecognised verb is also harmless, but is marked so the caller can see it.
void test_parser_handles_blanks_and_unknown() {
    for (const char* line : {"", "   ", "# comment", "   # indented comment", "\t"}) {
        FeedParseResult r = FeedParser::parse(line);
        CHECK(r.ok);
        CHECK(r.event.type == FeedEventType::NONE);
    }

    FeedParseResult u = FeedParser::parse("FLUSH 1 2 3");
    CHECK(u.ok);
    CHECK(u.event.type == FeedEventType::UNKNOWN);
    CHECK(u.event.verb == "FLUSH");
}

// The parser checks syntax, not sense. A zero quantity or a NaN price is a
// well formed line; the engine's validation gate is what refuses it. Keeping
// the split sharp means every refusal is visible as engine behaviour.
void test_parser_defers_semantics_to_the_engine() {
    CHECK(FeedParser::parse("ADD BUY LIMIT 100 0 1").ok);
    CHECK(FeedParser::parse("MODIFY 1 0").ok);
    CHECK(FeedParser::parse("ADD BUY LIMIT nan 5 1").ok);
    CHECK(FeedParser::parse("ADD BUY LIMIT inf 5 1").ok);
}

void test_parser_raw_line_preserved() {
    auto r = FeedParser::parse("   ADD BUY LIMIT 10 1 5   ");
    CHECK(r.ok);
    CHECK(r.event.raw == "   ADD BUY LIMIT 10 1 5   ");
}

// --- Pipeline: driving the book -------------------------------------------

// The spec's worked example, expressed as a feed.
void test_pipeline_matches_spec_example() {
    OrderBook book("T");
    Pipeline pipe(book);

    auto outcomes = pipe.submit_text(
        "ADD SELL LIMIT 105 100 1\n"
        "ADD SELL LIMIT 106 200 2\n"
        "ADD BUY LIMIT 105 150 3\n");

    CHECK(outcomes.size() == 3);
    for (const auto& o : outcomes) {
        CHECK(o.parsed);
        CHECK(o.applied);
    }

    const auto& last = outcomes[2];
    CHECK(last.order_id == 3);
    CHECK(last.filled_qty == 100);
    CHECK(last.leaves_qty == 50);
    CHECK(last.status == OrderStatus::PARTIALLY_FILLED);
    CHECK(last.trades.size() == 1);
    CHECK(last.avg_price.has_value() && near(*last.avg_price, 105.0));
    CHECK(near(*book.best_bid(), 105.0));
    CHECK(near(*book.best_ask(), 106.0));
    CHECK_VALID(book);
}

// The property that makes a feed safe to point at a live book: a line the
// parser cannot read changes nothing at all.
void test_malformed_line_touches_nothing() {
    OrderBook book("T");
    Pipeline pipe(book);

    pipe.submit_line("ADD SELL LIMIT 100 10 1");
    Analytics before = pipe.analytics();
    const auto events_before = pipe.order_events().size();
    const auto trades_before = pipe.trade_events().size();

    auto bad = pipe.submit_line("ADD BUY LIMIT not-a-price 5 2");
    CHECK(!bad.parsed);
    CHECK(!bad.applied);
    CHECK(!bad.error.empty());

    Analytics after = pipe.analytics();
    CHECK(after.lines_malformed == before.lines_malformed + 1);
    // No accepted work was recorded, and nothing was counted as an add.
    CHECK(after.adds == before.adds);
    CHECK(after.adds_accepted == before.adds_accepted);
    CHECK(after.adds_rejected == before.adds_rejected);
    CHECK(after.trades == before.trades);
    CHECK(pipe.order_events().size() == events_before);
    CHECK(pipe.trade_events().size() == trades_before);

    // And the book itself is exactly as it was.
    CHECK(book.total_orders() == 1);
    CHECK(near(*book.best_ask(), 100.0));
    CHECK(!book.best_bid().has_value());
    CHECK_VALID(book);
}

// A tape print is recorded, but must not become book activity: the engine's own
// executions are the authority a print is reconciled against.
void test_trade_print_is_recorded_but_not_matched() {
    OrderBook book("T");
    Pipeline pipe(book);

    auto o = pipe.submit_line("TRADE 100 5 11 12");
    CHECK(o.parsed);
    CHECK(o.applied);
    CHECK(o.type == FeedEventType::TRADE_PRINT);

    CHECK(pipe.trade_events().size() == 1);
    CHECK(pipe.trade_events()[0].from_print);
    CHECK(near(pipe.trade_events()[0].price, 100.0));

    // Book untouched.
    CHECK(book.total_orders() == 0);
    CHECK(book.trades().empty());
    CHECK(!book.best_bid().has_value());
    CHECK_VALID(book);

    // And excluded from the book's own statistics.
    Analytics a = pipe.analytics();
    CHECK(a.trade_prints == 1);
    CHECK(a.trades == 0);
    CHECK(a.total_volume == 0);
    CHECK(!a.has_trades);
    CHECK(near(a.vwap(), 0.0));
}

// A refused operation must be counted as refused. The engine reports refusal
// by return value, and a cancel aimed at a FILLED order fails while the order's
// status stays FILLED -- so reading the status would score it as a success.
void test_refused_operations_are_counted_as_refused() {
    OrderBook book("T");
    Pipeline pipe(book);

    pipe.submit_text(
        "ADD SELL LIMIT 100 10 1\n"
        "ADD SELL LIMIT 101 10 2\n"
        "ADD BUY LIMIT 101 15 3\n");

    // The buy takes the best (cheapest) ask first: all 10 of order 1 at 100,
    // then 5 of order 2 at 101. So order 1 and 3 are FILLED, and order 2 is
    // left working with 5.
    CHECK(book.get_order(1)->status == OrderStatus::FILLED);
    CHECK(book.get_order(2)->status == OrderStatus::PARTIALLY_FILLED);
    CHECK(book.get_order(3)->status == OrderStatus::FILLED);

    // Both of these aim at a FILLED order: the engine refuses, and the order's
    // status is left untouched at FILLED.
    auto bad_modify = pipe.submit_line("MODIFY 1 5");
    CHECK(bad_modify.parsed);
    CHECK(!bad_modify.applied);
    CHECK(book.get_order(1)->status == OrderStatus::FILLED);
    CHECK(book.get_order(1)->filled_qty == 10);

    auto bad_cancel = pipe.submit_line("CANCEL 3");
    CHECK(bad_cancel.parsed);
    CHECK(!bad_cancel.applied);
    CHECK(book.get_order(3)->status == OrderStatus::FILLED);

    // Order 2 is still working, so this one is accepted.
    auto ok_cancel = pipe.submit_line("CANCEL 2");
    CHECK(ok_cancel.applied);
    CHECK(book.get_order(2)->status == OrderStatus::CANCELLED);

    Analytics a = pipe.analytics();
    CHECK(a.modifies == 1);
    CHECK(a.modify_ok == 0);
    CHECK(a.modify_refused == 1);
    CHECK(a.cancels == 2);
    CHECK(a.cancel_ok == 1);
    CHECK(a.cancel_refused == 1);
    CHECK(a.cancelled_qty == 5);
}

// Engine refusals are distinguished from parser failures: both are "not
// applied", but only one is malformed input.
void test_engine_refusal_is_not_a_parse_error() {
    OrderBook book("T");
    Pipeline pipe(book);

    auto zero = pipe.submit_line("ADD BUY LIMIT 100 0 1");
    CHECK(zero.parsed);
    CHECK(!zero.applied);
    CHECK(zero.status == OrderStatus::REJECTED);

    auto nan = pipe.submit_line("ADD BUY LIMIT nan 5 2");
    CHECK(nan.parsed);
    CHECK(!nan.applied);

    auto dup = pipe.submit_line("ADD BUY LIMIT 100 5 1");
    CHECK(dup.parsed);
    CHECK(!dup.applied);

    Analytics a = pipe.analytics();
    CHECK(a.lines_malformed == 0);
    CHECK(a.adds == 3);
    CHECK(a.adds_accepted == 0);
    CHECK(a.adds_rejected == 3);
    CHECK_VALID(book);

    // The reason is recorded, not just the fact of refusal.
    const auto& evs = pipe.order_events();
    CHECK(evs[0].reject_code == RejectReason::QUANTITY_NOT_POSITIVE);
    CHECK(evs[1].reject_code == RejectReason::PRICE_NOT_FINITE);
    CHECK(evs[2].reject_code == RejectReason::DUPLICATE_ORDER_ID);
    CHECK(evs[0].applied == false);
}

void test_sequence_numbers_and_timestamps_propagate() {
    OrderBook book("T");
    Pipeline pipe(book);

    pipe.submit_text(
        "ADD SELL LIMIT 100 10 1 ts=111\n"
        "ADD BUY LIMIT 100 4 2 ts=222\n");

    const auto& evs = pipe.order_events();
    CHECK(evs.size() == 2);
    CHECK(evs[0].seq == 1);
    CHECK(evs[1].seq == 2);
    CHECK(evs[0].exchange_ts == 111);
    CHECK(evs[1].exchange_ts == 222);

    // The trade the second line caused carries the second line's timestamp.
    const auto& trs = pipe.trade_events();
    CHECK(trs.size() == 1);
    CHECK(trs[0].seq == 2);
    CHECK(trs[0].exchange_ts == 222);
    CHECK(trs[0].aggressor_id == 2);
    CHECK(trs[0].passive_id == 1);
    CHECK(trs[0].side == OrderSide::BUY);
    CHECK(!trs[0].from_print);

    // A wall clock stamp is present and plausible: after 2001 in microseconds.
    for (const auto& r : evs)
        CHECK(r.recorded_at_us > 1000000000000000ULL);
    CHECK(trs[0].recorded_at_us > 1000000000000000ULL);
}

void test_snapshots() {
    OrderBook book("T");
    PipelineConfig cfg;
    cfg.snapshot_depth = 3;
    Pipeline pipe(book, cfg);

    pipe.submit_text(
        "ADD SELL LIMIT 101 10 1\n"
        "ADD SELL LIMIT 102 10 2\n"
        "ADD BUY LIMIT 99 10 3\n");

    CHECK(pipe.snapshots().empty());
    pipe.take_snapshot();
    CHECK(pipe.snapshots().size() == 1);

    const auto& s = pipe.snapshots()[0];
    CHECK(s.bids.size() == 1);
    CHECK(s.asks.size() == 2);          // depth capped at 3, only 2 levels exist
    CHECK(s.bids[0].price == 99.0);
    CHECK(near(*s.best_bid, 99.0));
    CHECK(near(*s.best_ask, 101.0));
    CHECK(s.spread.has_value() && near(*s.spread, 2.0));
    CHECK(s.mid_price.has_value() && near(*s.mid_price, 100.0));
    CHECK(s.recorded_at_us > 1000000000000000ULL);
    CHECK(pipe.last_snapshot().bids.size() == 1);

    // Every-N snapshotting.
    OrderBook book2("T2");
    PipelineConfig cfg2;
    cfg2.snapshot_every = 2;
    Pipeline pipe2(book2, cfg2);
    pipe2.submit_text(
        "ADD SELL LIMIT 100 10 1\n"
        "ADD SELL LIMIT 101 10 2\n"
        "ADD SELL LIMIT 102 10 3\n"
        "ADD SELL LIMIT 103 10 4\n");
    CHECK(pipe2.snapshots().size() == 2);
    CHECK(pipe2.snapshots()[0].asks.size() == 2);
    CHECK(pipe2.snapshots()[1].asks.size() == 4);

    // A refused operation does not trigger a snapshot.
    OrderBook book3("T3");
    PipelineConfig cfg3;
    cfg3.snapshot_every = 1;
    Pipeline pipe3(book3, cfg3);
    pipe3.submit_line("CANCEL 999");
    CHECK(pipe3.snapshots().empty());
}

void test_analytics_on_a_hand_checked_book() {
    OrderBook book("T");
    Pipeline pipe(book);

    pipe.submit_text(
        "ADD SELL LIMIT 100 10 1\n"
        "ADD SELL LIMIT 101 10 2\n"
        "ADD BUY LIMIT 101 15 3\n");

    Analytics a = pipe.analytics();

    // The buy sweeps the asks from the best price up: 10 of order 1 at 100,
    // then 5 of order 2 at 101. Both with order 3 as aggressor.
    CHECK(a.trades == 2);
    CHECK(a.total_volume == 15);
    CHECK(near(a.total_notional, 1505.0));
    CHECK(near(a.vwap(), 1505.0 / 15.0));
    CHECK(a.buy_volume == 15);
    CHECK(a.sell_volume == 0);
    CHECK(near(a.high, 101.0));
    CHECK(near(a.low, 100.0));
    CHECK(a.largest_trade == 10);
    CHECK(a.volume_by_price.size() == 2);
    CHECK(a.volume_by_price[100.0] == 10);
    CHECK(a.volume_by_price[101.0] == 5);

    CHECK(a.adds == 3);
    CHECK(a.adds_accepted == 3);
    CHECK(a.submitted_qty == 35);
    CHECK(a.filled_qty == 15);
    CHECK(near(a.fill_rate(), 15.0 / 35.0));
    CHECK_VALID(book);
}

void test_analytics_two_sided_volume() {
    OrderBook book("T");
    Pipeline pipe(book);

    pipe.submit_text(
        "ADD BUY LIMIT 99 10 1\n"      // bid 99 x 10
        "ADD SELL LIMIT 99 4 2\n"      // SELL aggressor: 4 traded, 6 bid left
        "ADD SELL LIMIT 101 10 3\n"    // ask 101 x 10
        "ADD BUY LIMIT 101 6 4\n");    // BUY aggressor: 6 traded, 4 ask left

    Analytics a = pipe.analytics();
    CHECK(a.trades == 2);
    CHECK(a.total_volume == 10);
    CHECK(a.sell_volume == 4);       // order 2 hit the bid
    CHECK(a.buy_volume == 6);        // order 4 lifted the ask
    CHECK(near(a.total_notional, 1002.0));
    CHECK(near(a.vwap(), 100.2));
    CHECK(near(a.high, 101.0));
    CHECK(near(a.low, 99.0));
    CHECK(a.largest_trade == 6);
    CHECK(a.filled_qty == 10);
    CHECK(a.submitted_qty == 30);
    CHECK(near(a.fill_rate(), 10.0 / 30.0));
    CHECK_VALID(book);
}

void test_analytics_reports_line_tallies() {
    OrderBook book("T");
    Pipeline pipe(book);

    pipe.submit_text(
        "# a comment\n"
        "\n"
        "ADD SELL LIMIT 100 10 1\n"
        "GARBAGE here\n"
        "ADD BUY LIMIT 100 5 2\n");

    Analytics a = pipe.analytics();
    // Every line is accounted for exactly once.
    CHECK(a.lines_malformed == 0);
    CHECK(a.lines_parsed == 2);     // the two adds; the unrecognised verb is
    CHECK(a.lines_ignored == 3);    // comment, blank, unrecognised verb
    CHECK(a.lines_parsed + a.lines_ignored + a.lines_malformed == 5);
    CHECK(a.adds == 2);
    CHECK(a.adds_accepted == 2);
    CHECK_VALID(book);
}

// A line missing a field is malformed, and a malformed line is neither an add
// nor an accepted order.
void test_line_missing_a_field_is_malformed() {
    OrderBook book("T");
    Pipeline pipe(book);

    auto o = pipe.submit_line("ADD BUY LIMIT 100 5");   // no order id
    CHECK(!o.parsed);
    CHECK(!o.applied);
    CHECK(!o.error.empty());

    Analytics a = pipe.analytics();
    CHECK(a.lines_malformed == 1);
    CHECK(a.adds == 0);
    CHECK(book.total_orders() == 0);
    CHECK_VALID(book);
}

// Driving the book from a feed and then replaying the command log must land on
// the same book. This is the seam between the pipeline and features 1-3: if
// the feed path diverged from the replay path, reconstruction would be a lie.
void test_feed_then_replay_is_identical() {
    OrderBook live("T", 0.01);
    Pipeline pipe(live);

    pipe.submit_text(
        "# realistic-ish session\n"
        "ADD SELL LIMIT 105 100 1\n"
        "ADD SELL LIMIT 105 50 2\n"
        "ADD SELL LIMIT 106 200 3\n"
        "ADD BUY LIMIT 105 120 4 ts=1\n"
        "CANCEL 2\n"
        "ADD BUY LIMIT 104 40 5\n"
        "MODIFY 4 130\n"
        "ADD SELL LIMIT 104 60 6\n"
        "ADD BUY MARKET 25 7\n"
        "ADD SELL LIMIT 103 10 8\n"
        "CANCEL 1\n"
        "ADD BUY LIMIT 100 5 1\n"        // reuses a dead id: refused
        "ADD SELL LIMIT 1e999 5 9\n"     // inf price: refused by the engine
        "ADD SELL LIMIT 103.005 5 10\n");// off tick: refused

    CHECK_VALID(live);

    OrderBook rebuilt = replay("T", live.command_log().all(), 0.01);
    CHECK_VALID(rebuilt);
    CHECK(same_depth(live.depth(50), rebuilt.depth(50)));
    CHECK(live.total_orders() == rebuilt.total_orders());
    CHECK(live.sequence() == rebuilt.sequence());
    CHECK(live.bid_levels() == rebuilt.bid_levels());
    CHECK(live.ask_levels() == rebuilt.ask_levels());
    CHECK(live.trades().size() == rebuilt.trades().size());
    CHECK(near(live.total_notional(), rebuilt.total_notional()));

    for (const auto& id : live.queue(OrderSide::BUY, 104.0))
        CHECK(rebuilt.get_order(id) != nullptr);
}

void test_pipeline_applies_decoded_events_directly() {
    OrderBook book("T");
    Pipeline pipe(book);

    FeedEvent ev;
    ev.type     = FeedEventType::ADD;
    ev.side     = OrderSide::SELL;
    ev.order_type = OrderType::LIMIT;
    ev.price    = 100;
    ev.qty      = 10;
    ev.order_id = 42;

    auto o = pipe.apply(ev);
    CHECK(o.applied);
    CHECK(o.order_id == 42);
    CHECK(near(*book.best_ask(), 100.0));
    CHECK(pipe.analytics().adds == 1);
    CHECK_VALID(book);
}

int main() {
    test_parser_reads_each_verb();
    test_parser_extracts_timestamp();
    test_parser_rejects_malformed();
    test_parser_strips_inline_comments();
    test_parser_handles_blanks_and_unknown();
    test_parser_defers_semantics_to_the_engine();
    test_parser_raw_line_preserved();

    test_pipeline_matches_spec_example();
    test_malformed_line_touches_nothing();
    test_trade_print_is_recorded_but_not_matched();
    test_refused_operations_are_counted_as_refused();
    test_engine_refusal_is_not_a_parse_error();
    test_sequence_numbers_and_timestamps_propagate();
    test_snapshots();
    test_analytics_on_a_hand_checked_book();
    test_analytics_two_sided_volume();
    test_analytics_reports_line_tallies();
    test_line_missing_a_field_is_malformed();
    test_feed_then_replay_is_identical();
    test_pipeline_applies_decoded_events_directly();

    if (failures == 0) std::cout << "All tests passed\n";
    else               std::cout << failures << " test(s) FAILED\n";
    return failures == 0 ? 0 : 1;
}
