#include "order_book.h"

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

// The book must never be observably inconsistent, whatever was thrown at it.
#define CHECK_VALID(book) do { \
    std::string err; \
    if (!(book).validate(&err)) { \
        std::cerr << "FAIL: invariant violated: " << err << " @line " << __LINE__ << "\n"; \
        ++failures; \
    } \
} while (0)

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

// Queue order at one price, front first. Price-time priority is the whole
// point of the book, so assert on the real deque, not on acceptance order.
static std::string queue_at(const OrderBook& book, OrderSide side, Price price) {
    std::ostringstream oss;
    for (OrderId id : book.queue(side, price)) oss << id << " ";
    return oss.str();
}

void test_price_time_priority() {
    OrderBook book("T");
    OrderId b1 = book.add_limit_order(OrderSide::BUY, 100, 5).order_id;
    OrderId b2 = book.add_limit_order(OrderSide::BUY, 100, 7).order_id;
    OrderId b3 = book.add_limit_order(OrderSide::BUY, 99, 3).order_id;

    book.add_limit_order(OrderSide::SELL, 100, 6);

    const Order* o1 = book.get_order(b1);
    const Order* o2 = book.get_order(b2);
    const Order* o3 = book.get_order(b3);

    CHECK(o1->status == OrderStatus::FILLED);
    CHECK(o1->filled_qty == 5);
    CHECK(o2->status == OrderStatus::PARTIALLY_FILLED);
    CHECK(o2->filled_qty == 1);
    CHECK(o2->remaining() == 6);
    CHECK(o3->status == OrderStatus::OPEN);
    CHECK(o3->filled_qty == 0);

    CHECK_VALID(book);
}

void test_stop_when_no_cross() {
    OrderBook book("T");
    book.add_limit_order(OrderSide::BUY, 100, 10);
    book.add_limit_order(OrderSide::SELL, 105, 10);

    auto snap = book.snapshot();
    CHECK(snap.best_bid == 100);
    CHECK(snap.best_ask == 105);
    CHECK(snap.spread && *snap.spread == 5.0);
    CHECK(snap.mid_price && *snap.mid_price == 102.5);
}

void test_market_order_never_rests() {
    OrderBook book("T");
    book.add_limit_order(OrderSide::SELL, 100, 4);
    book.add_limit_order(OrderSide::SELL, 101, 5);

    OrderId m = book.add_market_order(OrderSide::BUY, 20).order_id;
    const Order* mo = book.get_order(m);

    CHECK(mo->status == OrderStatus::FILLED || mo->status == OrderStatus::PARTIALLY_FILLED);
    CHECK(mo->filled_qty == 9);
    CHECK(mo->remaining() == 11);
    CHECK(book.ask_levels() == 0);
    CHECK(!mo->is_resting());
    CHECK_VALID(book);
}

void test_cancel_then_future_match_skips_cancelled() {
    OrderBook book("T");
    OrderId b1 = book.add_limit_order(OrderSide::BUY, 100, 10).order_id;
    OrderId b2 = book.add_limit_order(OrderSide::BUY, 100, 10).order_id;

    CHECK(book.cancel_order(b1));
    const Order* o = book.get_order(b1);
    CHECK(o->status == OrderStatus::CANCELLED);

    book.add_limit_order(OrderSide::SELL, 100, 6);

    const Order* r = book.get_order(b2);
    CHECK(r->status == OrderStatus::PARTIALLY_FILLED);
    CHECK(r->filled_qty == 6);
    CHECK(r->remaining() == 4);
    CHECK_VALID(book);
}

void test_modify_increase_requeues_but_decrease_keeps_priority() {
    OrderBook book("T");
    OrderId a = book.add_limit_order(OrderSide::BUY, 100, 10).order_id;
    OrderId b = book.add_limit_order(OrderSide::BUY, 100, 10).order_id;
    book.add_limit_order(OrderSide::BUY, 100, 10);          // id 3
    CHECK(queue_at(book, OrderSide::BUY, 100) == "1 2 3 ");

    // Growing an order drops it to the back of the queue.
    CHECK(book.modify_order(a, 20));
    CHECK(queue_at(book, OrderSide::BUY, 100) == "2 3 1 ");

    // Shrinking must not buy a better position in line.
    CHECK(book.modify_order(b, 4));
    CHECK(queue_at(book, OrderSide::BUY, 100) == "2 3 1 ");

    // An increase after a shrink still requeues.
    CHECK(book.modify_order(b, 5));
    CHECK(queue_at(book, OrderSide::BUY, 100) == "3 1 2 ");

    CHECK(book.get_order(b)->qty == 5);
    CHECK(!book.modify_order(b, 0));
    CHECK_VALID(book);
}

void test_modify_rejects_invalid_quantities() {
    OrderBook book("T");
    OrderId b = book.add_limit_order(OrderSide::BUY, 100, 10).order_id;
    book.add_limit_order(OrderSide::SELL, 100, 4);   // leaves b partially filled

    const Order* o = book.get_order(b);
    CHECK(o->filled_qty == 4);
    CHECK(o->remaining() == 6);

    CHECK(!book.modify_order(b, 4));    // would contradict what already filled
    CHECK(!book.modify_order(b, 0));
    CHECK(!book.modify_order(9999, 5)); // unknown id
    CHECK(book.modify_order(b, 6));     // exactly the remainder is fine
    CHECK(book.get_order(b)->remaining() == 2);
    CHECK_VALID(book);
}

void test_modify_price_moves_level_and_loses_priority() {
    OrderBook book("T");
    OrderId a = book.add_limit_order(OrderSide::BUY, 100, 10).order_id;
    book.add_limit_order(OrderSide::BUY, 100, 10);          // id 2
    book.add_limit_order(OrderSide::BUY,  99, 10);          // id 3

    CHECK(book.modify_order(a, 10, 99.0));

    CHECK(book.get_order(a)->price == 99.0);
    CHECK(book.bid_levels() == 2);
    CHECK(book.best_bid() == 100.0);      // b is now the best bid
    CHECK(queue_at(book, OrderSide::BUY, 99) == "3 1 ");   // a requeued behind c
    CHECK(queue_at(book, OrderSide::BUY, 100) == "2 ");
    CHECK_VALID(book);

    // The old level must be gone entirely once its last order leaves.
    OrderId lone = book.add_limit_order(OrderSide::SELL, 200, 5).order_id;
    CHECK(book.modify_order(lone, 5, 201.0));
    CHECK(book.best_ask() == 201.0);
    CHECK(!book.best_bid() || *book.best_bid() < 201.0);
    CHECK_VALID(book);
}

void test_modify_cannot_resurrect_dead_orders() {
    OrderBook book("T");
    OrderId a = book.add_limit_order(OrderSide::BUY, 100, 10).order_id;
    CHECK(book.cancel_order(a));
    CHECK(!book.modify_order(a, 20));

    OrderId f = book.add_limit_order(OrderSide::BUY, 100, 5).order_id;
    book.add_limit_order(OrderSide::SELL, 100, 5);
    CHECK(book.get_order(f)->status == OrderStatus::FILLED);
    CHECK(!book.modify_order(f, 20));
    CHECK(!book.cancel_order(f));
}

void test_bbo_on_empty_book_is_empty_not_zero() {
    OrderBook book("T");
    CHECK(!book.best_bid().has_value());
    CHECK(!book.best_ask().has_value());
    CHECK(!book.mid_price().has_value());
    CHECK(!book.spread().has_value());

    // A one-sided book has a best price but no mid or spread, which is a
    // different thing from having a price of zero.
    book.add_limit_order(OrderSide::BUY, 100, 10);
    CHECK(book.best_bid().has_value());
    CHECK(*book.best_bid() == 100.0);
    CHECK(!book.best_ask().has_value());
    CHECK(!book.mid_price().has_value());   // one-sided book has no mid
    CHECK(!book.spread().has_value());
    CHECK_VALID(book);

    // Zero is not a tradeable price, so it can never masquerade as a real one.
    auto zero = book.add_limit_order(OrderSide::BUY, 0.0, 10);
    CHECK(zero.rejected());
    CHECK(zero.reject_code == RejectReason::PRICE_NOT_POSITIVE);
    CHECK(*book.best_bid() == 100.0);
    CHECK_VALID(book);
}

void test_depth_reports_price_order_and_aggregates() {
    OrderBook book("T");
    book.add_limit_order(OrderSide::BUY, 100, 5);
    book.add_limit_order(OrderSide::BUY, 100, 7);
    book.add_limit_order(OrderSide::BUY,  99, 3);
    book.add_limit_order(OrderSide::BUY,  98, 1);
    book.add_limit_order(OrderSide::SELL, 101, 4);
    book.add_limit_order(OrderSide::SELL, 102, 6);

    auto d = book.depth(2);
    CHECK(d.bids.size() == 2);
    CHECK(d.asks.size() == 2);

    CHECK(d.bids[0].price == 100);
    CHECK(d.bids[0].total_qty == 12);
    CHECK(d.bids[0].order_count == 2);
    CHECK(d.bids[1].price == 99);
    CHECK(d.bids[1].total_qty == 3);

    CHECK(d.asks[0].price == 101);
    CHECK(d.asks[0].total_qty == 4);
    CHECK(d.asks[1].price == 102);

    // A partial fill must reduce the level's displayed size.
    book.add_limit_order(OrderSide::SELL, 100, 6);
    auto d2 = book.depth(2);
    CHECK(d2.bids[0].price == 100);
    CHECK(d2.bids[0].total_qty == 6);
    CHECK(d2.bids[0].order_count == 1);
    CHECK_VALID(book);
}

void test_sequence_numbers_are_monotonic_and_unique() {
    OrderBook book("T");
    CHECK(book.sequence() == 0);
    OrderId a = book.add_limit_order(OrderSide::BUY, 100, 10).order_id;
    OrderId b = book.add_limit_order(OrderSide::SELL, 105, 10).order_id;
    CHECK(book.sequence() == 2);
    CHECK(book.get_order(a)->seq == 1);
    CHECK(book.get_order(b)->seq == 2);

    book.cancel_order(a);
    CHECK(book.sequence() == 3);

    // Every accepted request gets its own number.
    SeqNum last = 0;
    for (const auto& c : book.command_log().all()) {
        CHECK(c.seq > last);
        last = c.seq;
    }
    CHECK(last == book.sequence());
}

void test_trades_recorded_and_events_ordered() {
    OrderBook book("T");
    book.add_limit_order(OrderSide::BUY, 100, 10);
    book.add_limit_order(OrderSide::SELL, 100, 10);

    const auto& ev = book.event_stream();
    CHECK(ev.size() == 5);

    size_t adds = 0, trades = 0, fills = 0, cancels = 0;
    for (const auto& e : ev.all()) {
        if (std::holds_alternative<OrderAdded>(e)) ++adds;
        else if (std::holds_alternative<TradeEvent>(e)) ++trades;
        else if (std::holds_alternative<OrderFilled>(e)) ++fills;
        else if (std::holds_alternative<OrderCancelled>(e)) ++cancels;
    }
    CHECK(adds == 2);
    CHECK(trades == 1);
    CHECK(fills == 2);
    CHECK(cancels == 0);
}

void test_trade_keeps_passive_order_price() {
    OrderBook book("T");
    book.add_limit_order(OrderSide::SELL, 100, 5);
    book.add_limit_order(OrderSide::SELL, 101, 5);
    book.add_limit_order(OrderSide::BUY, 101, 8);   // sweeps both levels

    const auto& ev = book.event_stream().all();
    size_t trades = 0;
    for (const auto& e : ev) {
        if (!std::holds_alternative<TradeEvent>(e)) continue;
        ++trades;
        const Trade& t = std::get<TradeEvent>(e).trade;
        CHECK(t.side == OrderSide::BUY);            // aggressor side
        if (t.passive_id == 1) CHECK(t.price == 100);
        if (t.passive_id == 2) CHECK(t.price == 101);
        CHECK(t.qty > 0);
    }
    CHECK(trades == 2);
}

void test_replay_reconstructs_identical_book() {
    OrderBook book("T");

    OrderId a = book.add_limit_order(OrderSide::BUY, 100, 10).order_id;
    OrderId b = book.add_limit_order(OrderSide::BUY, 100, 10).order_id;
    OrderId c = book.add_limit_order(OrderSide::BUY,  99, 10).order_id;
    OrderId d = book.add_limit_order(OrderSide::SELL, 101, 10).order_id;
    OrderId e = book.add_limit_order(OrderSide::SELL, 102, 10).order_id;
    (void)b; (void)c; (void)d; (void)e;

    book.modify_order(a, 25);
    book.cancel_order(d);
    book.add_limit_order(OrderSide::SELL, 100, 6);
    book.modify_order(e, 4, 101.0);
    book.add_market_order(OrderSide::BUY, 7);
    book.add_limit_order(OrderSide::BUY, 103, 1);
    book.cancel_order(1);
    book.modify_order(c, 3);
    book.add_limit_order(OrderSide::SELL, 104, 2);

    OrderBook rebuilt = replay("T", book.command_log().all());

    std::string err;
    CHECK(rebuilt.validate(&err));
    CHECK(same_depth(book.depth(50), rebuilt.depth(50)));
    CHECK(book.total_orders() == rebuilt.total_orders());
    CHECK(book.bid_levels() == rebuilt.bid_levels());
    CHECK(book.ask_levels() == rebuilt.ask_levels());
    CHECK(book.sequence() == rebuilt.sequence());

    // Same queue order, not merely the same totals.
    for (OrderId id = 1; id <= book.total_orders(); ++id) {
        const Order* x = book.get_order(id);
        const Order* y = rebuilt.get_order(id);
        CHECK(x && y);
        if (!x || !y) continue;
        CHECK(x->status == y->status);
        CHECK(x->filled_qty == y->filled_qty);
        CHECK(x->price == y->price);
        CHECK(x->side == y->side);
    }
    CHECK(queue_at(book, OrderSide::BUY, 99) == queue_at(rebuilt, OrderSide::BUY, 99));
    CHECK(queue_at(book, OrderSide::BUY, 101) == queue_at(rebuilt, OrderSide::BUY, 101));

    // Replaying the rebuilt book must be a fixed point.
    OrderBook again = replay("T", rebuilt.command_log().all());
    CHECK(same_depth(rebuilt.depth(50), again.depth(50)));
    CHECK(again.command_log().size() == rebuilt.command_log().size());
}

// Invariants have to hold through a long random stream, not just tidy cases.
void test_invariants_hold_under_random_stream() {
    OrderBook book("T");
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    auto next = [&rng]() {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        return rng;
    };

    std::vector<OrderId> live;
    for (int i = 0; i < 4000; ++i) {
        uint64_t r = next();
        int action = static_cast<int>(r % 100);

        if (action < 55 || live.empty()) {
            OrderSide side = (r & (1ull << 20)) ? OrderSide::BUY : OrderSide::SELL;
            // Sprinkle in invalid quantities and prices to fuzz the rejection
            // path, including non-finite prices that must never reach a level.
            Quantity qty = (next() % 40 == 0) ? 0 : 1 + next() % 20;
            uint64_t pr = next();
            Price px;
            if (pr % 37 == 0)      px = std::numeric_limits<double>::quiet_NaN();
            else if (pr % 41 == 0) px = -1.0;
            else if (pr % 43 == 0) px = std::numeric_limits<double>::infinity();
            else                   px = 95.0 + static_cast<double>(next() % 11);
            OrderId id = book.add_limit_order(side, px, qty).order_id;
            if (book.get_order(id)->is_working()) live.push_back(id);
        } else if (action < 70) {
            size_t k = next() % live.size();
            OrderId id = live[k];
            live[k] = live.back();
            live.pop_back();
            book.cancel_order(id);
        } else if (action < 85) {
            OrderId id = live[next() % live.size()];
            Quantity q = 1 + next() % 30;
            book.modify_order(id, q);
        } else if (action < 92) {
            OrderSide side = (r & (1ull << 21)) ? OrderSide::BUY : OrderSide::SELL;
            Quantity mq = (next() % 40 == 0) ? 0 : 1 + next() % 25;
            OrderId id = book.add_market_order(side, mq).order_id;
            if (book.get_order(id)->is_working()) live.push_back(id);
        } else {
            OrderId id = live[next() % live.size()];
            uint64_t pr = next();
            Price px;
            if (pr % 37 == 0)      px = std::numeric_limits<double>::quiet_NaN();
            else if (pr % 41 == 0) px = -1.0;
            else                   px = 95.0 + static_cast<double>(next() % 11);
            book.modify_order(id, 1 + next() % 30, px);
        }

        std::string err;
        if (!book.validate(&err)) {
            std::cerr << "FAIL: invariant broke at step " << i << ": " << err << "\n";
            ++failures;
            break;
        }
    }

    // A replay of a book that survived 4000 random requests must still match.
    OrderBook rebuilt = replay("T", book.command_log().all());
    std::string err;
    CHECK(rebuilt.validate(&err));
    CHECK(same_depth(book.depth(1000), rebuilt.depth(1000)));
}

// A price change that crosses must trade, not rest on top of the offer.
void test_modify_price_that_crosses_matches_immediately() {
    OrderBook book("T");
    OrderId b = book.add_limit_order(OrderSide::BUY, 90, 10).order_id;
    book.add_limit_order(OrderSide::SELL, 100, 4);
    book.add_limit_order(OrderSide::SELL, 101, 4);

    CHECK(book.get_order(b)->price == 90);
    CHECK(book.best_bid() == 90);

    // Reposition the bid to 101: it should sweep both offers rather than
    // leaving the book crossed.
    CHECK(book.modify_order(b, 10, 101.0));

    const Order* o = book.get_order(b);
    CHECK(o->filled_qty == 8);
    CHECK(o->remaining() == 2);
    CHECK(o->status == OrderStatus::PARTIALLY_FILLED);
    CHECK(!book.best_ask().has_value() || *book.best_ask() > 101.0);
    CHECK(*book.best_bid() == 101.0);
    CHECK(book.ask_levels() == 0);
    CHECK(book.bid_levels() == 1);
    CHECK(o->is_resting());
    CHECK(book.queue(OrderSide::BUY, 101.0).size() == 1);
    CHECK_VALID(book);

    // Repositioning far enough to fill completely leaves nothing resting.
    OrderId c = book.add_limit_order(OrderSide::BUY, 90, 5).order_id;
    book.add_limit_order(OrderSide::SELL, 102, 5);
    CHECK(book.modify_order(c, 5, 102.0));
    CHECK(book.get_order(c)->status == OrderStatus::FILLED);
    CHECK(!book.get_order(c)->is_resting());
    CHECK(book.queue(OrderSide::BUY, 102.0).empty());
    CHECK(book.bid_levels() == 1);   // only the earlier bid remains
    CHECK_VALID(book);
}

// The worked example from the spec:
//   ASK 105 -> 100, ASK 106 -> 200, BUY 150 @ 105
void test_spec_worked_example() {
    OrderBook book("T");
    book.add_limit_order(OrderSide::SELL, 105, 100);
    book.add_limit_order(OrderSide::SELL, 106, 200);

    auto r = book.add_limit_order(OrderSide::BUY, 105, 150);

    CHECK(r.accepted());
    CHECK(r.filled_qty == 100);
    CHECK(r.leaves_qty == 50);
    CHECK(r.status == OrderStatus::PARTIALLY_FILLED);
    CHECK(r.match_count() == 1);
    CHECK(r.fills[0].price == 105);
    CHECK(r.fills[0].qty == 100);

    // A limit bid at 105 may not lift an offer at 106, so that level is
    // untouched. (The spec lists 106 -> 200, which is only consistent with
    // the buy not reaching it.)
    auto d = book.depth(5);
    CHECK(d.asks.size() == 1);
    CHECK(d.asks[0].price == 106);
    CHECK(d.asks[0].total_qty == 200);

    // The unfilled 50 rests as a bid at 105, so 105 shows on both sides.
    CHECK(d.bids.size() == 1);
    CHECK(d.bids[0].price == 105);
    CHECK(d.bids[0].total_qty == 50);
    CHECK(book.ask_levels() == 1);
    CHECK(book.bid_levels() == 1);
    CHECK(book.get_order(r.order_id)->is_resting());
    CHECK_VALID(book);
}

void test_multiple_matches_across_levels() {
    OrderBook book("T");
    book.add_limit_order(OrderSide::SELL, 105, 100);
    book.add_limit_order(OrderSide::SELL, 106, 200);

    auto r = book.add_limit_order(OrderSide::BUY, 106, 150);

    CHECK(r.filled_qty == 150);
    CHECK(r.leaves_qty == 0);
    CHECK(r.status == OrderStatus::FILLED);
    CHECK(r.fully_filled());
    CHECK(r.match_count() == 2);
    CHECK(r.fills[0].price == 105);
    CHECK(r.fills[0].qty == 100);
    CHECK(r.fills[1].price == 106);
    CHECK(r.fills[1].qty == 50);

    // Cheapest price first, always.
    CHECK(r.fills[0].price < r.fills[1].price);

    double expected = (105.0 * 100 + 106.0 * 50) / 150.0;
    CHECK(r.avg_price().has_value());
    CHECK(std::abs(*r.avg_price() - expected) < 1e-9);

    // The 106 level keeps what was not taken.
    CHECK(book.ask_levels() == 1);
    CHECK(book.depth(5).asks[0].total_qty == 150);
    CHECK(!book.get_order(r.order_id)->is_resting());
    CHECK(book.bid_levels() == 0);
    CHECK_VALID(book);
}

void test_price_time_priority_during_matching() {
    OrderBook book("T");
    OrderId a = book.add_limit_order(OrderSide::SELL, 100, 5).order_id;
    OrderId b = book.add_limit_order(OrderSide::SELL, 100, 5).order_id;
    OrderId c = book.add_limit_order(OrderSide::SELL, 100, 5).order_id;

    auto r = book.add_limit_order(OrderSide::BUY, 100, 12);

    // Oldest first, even though the last one is only partly taken.
    CHECK(r.match_count() == 3);
    CHECK(r.fills[0].resting_id == a);
    CHECK(r.fills[1].resting_id == b);
    CHECK(r.fills[2].resting_id == c);
    CHECK(r.fills[0].qty == 5);
    CHECK(r.fills[1].qty == 5);
    CHECK(r.fills[2].qty == 2);

    CHECK(book.get_order(a)->status == OrderStatus::FILLED);
    CHECK(book.get_order(b)->status == OrderStatus::FILLED);
    CHECK(book.get_order(c)->status == OrderStatus::PARTIALLY_FILLED);
    CHECK(book.get_order(c)->remaining() == 3);
    CHECK(book.queue(OrderSide::SELL, 100).size() == 1);
    CHECK_VALID(book);
}

void test_sell_aggressor_sweeps_bids_best_first() {
    OrderBook book("T");
    book.add_limit_order(OrderSide::BUY,  99, 50);
    book.add_limit_order(OrderSide::BUY, 100, 50);
    book.add_limit_order(OrderSide::BUY, 101, 50);

    auto r = book.add_limit_order(OrderSide::SELL, 100, 120);

    CHECK(r.match_count() == 2);
    CHECK(r.fills[0].price == 101);
    CHECK(r.fills[0].qty == 50);
    CHECK(r.fills[1].price == 100);
    CHECK(r.fills[1].qty == 50);
    CHECK(r.filled_qty == 100);
    CHECK(r.leaves_qty == 20);
    CHECK(r.status == OrderStatus::PARTIALLY_FILLED);

    // 99 is out of reach, and the remainder rests as an offer at 100.
    CHECK(book.best_bid() == 99.0);
    CHECK(book.best_ask() == 100.0);
    CHECK(book.depth(5).asks[0].total_qty == 20);
    CHECK_VALID(book);
}

void test_trade_generation_and_journal() {
    OrderBook book("T");
    OrderId resting_a = book.add_limit_order(OrderSide::SELL, 105, 100).order_id;
    OrderId resting_b = book.add_limit_order(OrderSide::SELL, 106, 200).order_id;

    SeqNum before = book.sequence();
    auto r = book.add_limit_order(OrderSide::BUY, 106, 150);

    const auto& all = book.trades();
    CHECK(all.size() == 2);
    CHECK(all[0].trade_id == 1);
    CHECK(all[1].trade_id == 2);
    CHECK(all[0].aggressor_id == r.order_id);
    CHECK(all[0].passive_id == resting_a);
    CHECK(all[1].passive_id == resting_b);
    CHECK(all[0].side == OrderSide::BUY);
    CHECK(all[0].qty == 100);
    CHECK(all[1].qty == 50);

    // trades_since isolates the output of one submission.
    auto mine = book.trades_since(before);
    CHECK(mine.size() == 2);
    CHECK(mine[0].seq == r.seq);
    CHECK(book.trades_since(book.sequence()).empty());

    // The passive side sees the same fills from its own point of view.
    CHECK(book.get_order(resting_a)->filled_qty == 100);
    CHECK(book.get_order(resting_a)->status == OrderStatus::FILLED);
    CHECK(book.get_order(resting_b)->filled_qty == 50);
    CHECK(book.get_order(resting_b)->status == OrderStatus::PARTIALLY_FILLED);
    CHECK_VALID(book);
}

void test_partial_fill_keeps_queue_position() {
    OrderBook book("T");
    OrderId a = book.add_limit_order(OrderSide::SELL, 100, 10).order_id;
    OrderId b = book.add_limit_order(OrderSide::SELL, 100, 10).order_id;

    book.add_limit_order(OrderSide::BUY, 100, 4);

    // Being partly filled must not cost an order its place in line.
    auto q = book.queue(OrderSide::SELL, 100);
    CHECK(q.size() == 2);
    CHECK(q[0] == a);
    CHECK(q[1] == b);

    auto d = book.depth(1);
    CHECK(d.asks[0].total_qty == 16);
    CHECK(d.asks[0].order_count == 2);
    CHECK_VALID(book);
}

void test_market_order_sweeps_multiple_levels() {
    OrderBook book("T");
    book.add_limit_order(OrderSide::SELL, 100, 10);
    book.add_limit_order(OrderSide::SELL, 101, 10);
    book.add_limit_order(OrderSide::SELL, 102, 10);

    auto r = book.add_market_order(OrderSide::BUY, 25);
    CHECK(r.filled_qty == 25);
    CHECK(r.match_count() == 3);
    CHECK(r.fills[0].price == 100);
    CHECK(r.fills[1].price == 101);
    CHECK(r.fills[2].price == 102);
    CHECK(r.fills[2].qty == 5);

    double expected = (100.0 * 10 + 101.0 * 10 + 102.0 * 5) / 25.0;
    CHECK(r.avg_price().has_value());
    CHECK(std::abs(*r.avg_price() - expected) < 1e-9);

    CHECK(book.ask_levels() == 1);
    CHECK(book.depth(5).asks[0].total_qty == 5);
    CHECK(!book.get_order(r.order_id)->is_resting());

    // Nothing on the other side to hit.
    auto none = book.add_market_order(OrderSide::SELL, 5);
    CHECK(none.accepted());
    CHECK(none.filled_qty == 0);
    CHECK(none.match_count() == 0);
    CHECK(!none.avg_price().has_value());
    CHECK_VALID(book);
}

void test_zero_quantity_is_rejected_not_corrupting() {
    OrderBook book("T");
    book.add_limit_order(OrderSide::BUY, 100, 10);
    book.add_limit_order(OrderSide::SELL, 105, 10);

    auto r = book.add_limit_order(OrderSide::BUY, 100, 0);
    CHECK(!r.accepted());
    CHECK(r.status == OrderStatus::REJECTED);
    CHECK(r.filled_qty == 0);
    CHECK(r.leaves_qty == 0);
    CHECK(r.match_count() == 0);
    CHECK(!r.avg_price().has_value());
    CHECK(!r.reject_reason().empty());
    CHECK(book.get_order(r.order_id)->status == OrderStatus::REJECTED);
    CHECK(book.trades().size() == 0);
    CHECK(book.total_orders() == 3);
    CHECK_VALID(book);

    auto m = book.add_market_order(OrderSide::BUY, 0);
    CHECK(!m.accepted());
    CHECK_VALID(book);

    // A rejected submission must leave the book fully functional.
    auto ok = book.add_limit_order(OrderSide::SELL, 100, 4);
    CHECK(ok.accepted());
    CHECK(ok.filled_qty == 4);
    CHECK(ok.match_count() == 1);
    CHECK_VALID(book);
}

void test_order_status_through_lifecycle() {
    OrderBook book("T");

    auto resting = book.add_limit_order(OrderSide::BUY, 100, 10);
    CHECK(resting.status == OrderStatus::OPEN);
    CHECK(resting.leaves_qty == 10);
    CHECK(!resting.avg_price().has_value());

    book.add_limit_order(OrderSide::SELL, 100, 4);
    CHECK(book.get_order(resting.order_id)->status == OrderStatus::PARTIALLY_FILLED);
    CHECK(book.get_order(resting.order_id)->remaining() == 6);

    book.add_limit_order(OrderSide::SELL, 100, 6);
    CHECK(book.get_order(resting.order_id)->status == OrderStatus::FILLED);

    // An aggressor that fills outright reports FILLED, not PARTIAL.
    book.add_limit_order(OrderSide::BUY, 90, 5);
    auto full = book.add_limit_order(OrderSide::SELL, 90, 5);
    CHECK(full.status == OrderStatus::FILLED);
    CHECK(full.leaves_qty == 0);
    CHECK(full.filled_qty == 5);

    OrderId c = book.add_limit_order(OrderSide::BUY, 80, 5).order_id;
    book.cancel_order(c);
    CHECK(book.get_order(c)->status == OrderStatus::CANCELLED);
    CHECK_VALID(book);
}

void test_lifecycle_transition_table() {
    using S = OrderStatus;

    // A terminal state is final: nothing reopens a finished order.
    CHECK(is_terminal(S::FILLED));
    CHECK(is_terminal(S::CANCELLED));
    CHECK(is_terminal(S::REJECTED));
    CHECK(!is_terminal(S::NEW));
    CHECK(!is_terminal(S::OPEN));
    CHECK(!is_terminal(S::PARTIALLY_FILLED));

    CHECK(!can_transition_to(S::FILLED, S::OPEN));
    CHECK(!can_transition_to(S::FILLED, S::PARTIALLY_FILLED));
    CHECK(!can_transition_to(S::CANCELLED, S::OPEN));
    CHECK(!can_transition_to(S::REJECTED, S::OPEN));

    // NEW -> OPEN -> PARTIALLY_FILLED -> FILLED
    CHECK(can_transition_to(S::NEW, S::OPEN));
    CHECK(can_transition_to(S::OPEN, S::PARTIALLY_FILLED));
    CHECK(can_transition_to(S::PARTIALLY_FILLED, S::PARTIALLY_FILLED));
    CHECK(can_transition_to(S::PARTIALLY_FILLED, S::FILLED));

    // NEW -> OPEN -> CANCELLED
    CHECK(can_transition_to(S::OPEN, S::CANCELLED));
    CHECK(can_transition_to(S::PARTIALLY_FILLED, S::CANCELLED));

    // An order may be matched the instant it is accepted, skipping OPEN.
    CHECK(can_transition_to(S::NEW, S::FILLED));
    CHECK(can_transition_to(S::NEW, S::PARTIALLY_FILLED));
    CHECK(can_transition_to(S::NEW, S::REJECTED));

    // A working order cannot go backwards to NEW.
    CHECK(!can_transition_to(S::OPEN, S::NEW));
    CHECK(!can_transition_to(S::PARTIALLY_FILLED, S::NEW));
    CHECK(!can_transition_to(S::OPEN, S::REJECTED));
}

void test_order_lifecycle_happy_paths() {
    OrderBook book("T");

    // NEW -> OPEN -> PARTIALLY_FILLED -> FILLED
    auto o = book.add_limit_order(OrderSide::BUY, 100, 10);
    const Order* p = book.get_order(o.order_id);
    CHECK(o.status == OrderStatus::OPEN);
    CHECK(p->is_working());
    CHECK(p->is_accepted());
    CHECK(!p->is_terminal());
    CHECK(p->is_resting());

    book.add_limit_order(OrderSide::SELL, 100, 4);
    p = book.get_order(o.order_id);
    CHECK(p->status == OrderStatus::PARTIALLY_FILLED);
    CHECK(p->is_working());
    CHECK(p->is_resting());
    CHECK(!p->is_terminal());
    CHECK(p->remaining() == 6);

    book.add_limit_order(OrderSide::SELL, 100, 6);
    p = book.get_order(o.order_id);
    CHECK(p->status == OrderStatus::FILLED);
    CHECK(p->is_terminal());
    CHECK(p->is_accepted());
    CHECK(!p->is_working());
    CHECK(!p->is_resting());

    // NEW -> OPEN -> CANCELLED
    auto c = book.add_limit_order(OrderSide::BUY, 90, 5);
    CHECK(book.get_order(c.order_id)->status == OrderStatus::OPEN);
    CHECK(book.cancel_order(c.order_id));
    p = book.get_order(c.order_id);
    CHECK(p->status == OrderStatus::CANCELLED);
    CHECK(p->is_terminal());
    CHECK(p->is_accepted());      // it was accepted, then withdrawn
    CHECK(!p->is_working());
    CHECK_VALID(book);
}

void test_order_history_records_every_step() {
    OrderBook book("T");
    auto o = book.add_limit_order(OrderSide::BUY, 100, 10);
    book.add_limit_order(OrderSide::SELL, 100, 4);
    book.add_limit_order(OrderSide::SELL, 100, 6);

    const auto& h = book.order_history(o.order_id);
    CHECK(h.size() == 3);
    CHECK(h[0].from == OrderStatus::NEW);
    CHECK(h[0].to == OrderStatus::OPEN);
    CHECK(h[1].from == OrderStatus::OPEN);
    CHECK(h[1].to == OrderStatus::PARTIALLY_FILLED);
    CHECK(h[2].from == OrderStatus::PARTIALLY_FILLED);
    CHECK(h[2].to == OrderStatus::FILLED);
    for (size_t i = 1; i < h.size(); ++i) CHECK(h[i].seq > h[i - 1].seq);
    CHECK(h.back().to == book.get_order(o.order_id)->status);

    auto c = book.add_limit_order(OrderSide::BUY, 90, 5).order_id;
    book.cancel_order(c);
    const auto& hc = book.order_history(c);
    CHECK(hc.size() == 2);
    CHECK(hc[1].to == OrderStatus::CANCELLED);

    // A refusal is a step in the life too, and says so.
    auto r = book.add_limit_order(OrderSide::BUY, -1.0, 5);
    const auto& hr = book.order_history(r.order_id);
    CHECK(hr.size() == 1);
    CHECK(hr[0].from == OrderStatus::NEW);
    CHECK(hr[0].to == OrderStatus::REJECTED);

    CHECK(book.order_history(99999).empty());
    CHECK_VALID(book);
}

void test_order_validation_rejects_bad_submissions() {
    OrderBook book("T");
    book.add_limit_order(OrderSide::BUY, 100, 10);
    book.add_limit_order(OrderSide::SELL, 105, 10);

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    auto expect_refused = [](const ExecutionReport& r, RejectReason why) {
        CHECK(!r.accepted());
        CHECK(r.status == OrderStatus::REJECTED);
        CHECK(r.reject_code == why);
        CHECK(r.filled_qty == 0);
        CHECK(r.leaves_qty == 0);
        CHECK(r.match_count() == 0);
        CHECK(!r.avg_price().has_value());
        CHECK(!r.reject_reason().empty());
    };

    expect_refused(book.add_limit_order(OrderSide::BUY, 100, 0),
                   RejectReason::QUANTITY_NOT_POSITIVE);
    expect_refused(book.add_limit_order(OrderSide::BUY, -1.0, 10),
                   RejectReason::PRICE_NOT_POSITIVE);
    expect_refused(book.add_limit_order(OrderSide::BUY, 0.0, 10),
                   RejectReason::PRICE_NOT_POSITIVE);
    expect_refused(book.add_limit_order(OrderSide::BUY, nan, 10),
                   RejectReason::PRICE_NOT_FINITE);
    expect_refused(book.add_limit_order(OrderSide::BUY, inf, 10),
                   RejectReason::PRICE_NOT_FINITE);
    expect_refused(book.add_limit_order(OrderSide::SELL, -inf, 10),
                   RejectReason::PRICE_NOT_FINITE);
    expect_refused(book.add_market_order(OrderSide::BUY, 0),
                   RejectReason::QUANTITY_NOT_POSITIVE);

    // A non-finite price must never reach a level: std::map<double> with a NaN
    // key silently corrupts iteration and depth.
    CHECK(book.total_orders() == 9);
    CHECK(book.trades().size() == 0);
    CHECK(book.bid_levels() == 1);
    CHECK(book.ask_levels() == 1);
    CHECK(*book.best_bid() == 100.0);
    CHECK(*book.best_ask() == 105.0);
    CHECK_VALID(book);

    // A modify cannot smuggle in a bad price either.
    auto o = book.add_limit_order(OrderSide::BUY, 50, 5);
    CHECK(!book.modify_order(o.order_id, 5, -1.0));
    CHECK(!book.modify_order(o.order_id, 5, nan));
    CHECK(!book.modify_order(o.order_id, 5, inf));
    CHECK(book.get_order(o.order_id)->price == 50.0);
    CHECK(book.get_order(o.order_id)->status == OrderStatus::OPEN);
    CHECK(book.get_order(o.order_id)->is_resting());
    CHECK_VALID(book);
}

void test_unique_order_ids_and_idempotent_retry() {
    OrderBook book("T");

    auto a = book.add_limit_order(OrderSide::BUY, 100, 10, 500);
    CHECK(a.order_id == 500);
    CHECK(a.accepted());

    // The same id again is refused rather than silently overwriting the first.
    auto dup = book.add_limit_order(OrderSide::BUY, 101, 10, 500);
    CHECK(dup.rejected());
    CHECK(dup.reject_code == RejectReason::DUPLICATE_ORDER_ID);
    CHECK(book.get_order(500)->price == 100.0);
    CHECK(book.get_order(500)->qty == 10);
    CHECK(book.bid_levels() == 1);
    CHECK_VALID(book);

    // Auto-assigned ids must not collide with explicit ones, so they are
    // pushed above any id the caller has already used.
    auto b = book.add_limit_order(OrderSide::BUY, 99, 5);
    CHECK(b.order_id > 500);
    auto c = book.add_limit_order(OrderSide::SELL, 200, 5, 5000);
    CHECK(c.order_id == 5000);
    auto d = book.add_limit_order(OrderSide::SELL, 201, 5);
    CHECK(d.order_id == 5001);
    // Four live orders: the refused duplicate never became a second order,
    // because the id was already taken.
    CHECK(book.total_orders() == 4);
    CHECK(book.get_order(500)->status == OrderStatus::OPEN);
    CHECK_VALID(book);
}

void test_tick_size_validation() {
    OrderBook book("T", 0.25);
    CHECK(book.tick_size() == 0.25);

    CHECK(book.add_limit_order(OrderSide::BUY, 100.25, 10).accepted());
    CHECK(book.add_limit_order(OrderSide::SELL, 100.75, 10).accepted());

    auto off = book.add_limit_order(OrderSide::BUY, 100.10, 10);
    CHECK(off.rejected());
    CHECK(off.reject_code == RejectReason::OFF_TICK);

    // Repricing off the grid is refused, and leaves the order untouched.
    auto o = book.add_limit_order(OrderSide::BUY, 99.75, 10);
    CHECK(!book.modify_order(o.order_id, 10, 100.30));
    CHECK(book.get_order(o.order_id)->price == 99.75);
    CHECK(book.get_order(o.order_id)->status == OrderStatus::OPEN);
    CHECK(!book.modify_order(o.order_id, 10, 100.55));
    CHECK(book.get_order(o.order_id)->price == 99.75);
    // A price on the grid is still accepted.
    CHECK(book.modify_order(o.order_id, 10, 99.50));
    CHECK(book.get_order(o.order_id)->price == 99.50);
    CHECK_VALID(book);

    // A book with no tick size accepts any valid price.
    OrderBook free("F", 0.0);
    CHECK(free.tick_size() == 0.0);
    CHECK(free.add_limit_order(OrderSide::BUY, 100.125, 10).accepted());
    CHECK_VALID(free);
}

void test_trade_history_queries() {
    OrderBook book("T");
    auto a = book.add_limit_order(OrderSide::SELL, 105, 100);
    auto b = book.add_limit_order(OrderSide::SELL, 106, 200);
    auto t = book.add_limit_order(OrderSide::BUY, 106, 150);

    CHECK(book.trades_for_order(t.order_id).size() == 2);
    CHECK(book.trades_for_order(a.order_id).size() == 1);
    CHECK(book.trades_for_order(b.order_id).size() == 1);
    CHECK(book.trades_for_order(9999).empty());

    CHECK(book.total_volume() == 150);
    CHECK(std::abs(book.total_notional() - (105.0 * 100 + 106.0 * 50)) < 1e-9);
    CHECK_VALID(book);

    // An order can be filled as the passive side and later reprice to become
    // the aggressor, so its history spans both.
    OrderBook two("T2");
    auto x = two.add_limit_order(OrderSide::BUY, 100, 10);   // rests
    two.add_limit_order(OrderSide::SELL, 100, 4);           // x trades passively
    CHECK(two.trades_for_order(x.order_id).size() == 1);
    CHECK(two.trades_for_order(x.order_id)[0].passive_id == x.order_id);

    two.add_limit_order(OrderSide::SELL, 105, 3);
    CHECK(two.modify_order(x.order_id, 10, 105.0));         // now it crosses
    const auto& xt = two.trades_for_order(x.order_id);
    CHECK(xt.size() == 2);
    CHECK(xt[0].passive_id == x.order_id);
    CHECK(xt[1].aggressor_id == x.order_id);
    CHECK(two.total_volume() == 7);
    CHECK_VALID(two);
}

void test_replay_preserves_tick_size_and_rejections() {
    OrderBook book("T", 0.5);
    book.add_limit_order(OrderSide::BUY, 100.0, 10);
    book.add_limit_order(OrderSide::SELL, 105.0, 20);
    book.add_limit_order(OrderSide::BUY, 100.3, 5);     // off the grid
    book.add_limit_order(OrderSide::SELL, 104.5, 5);
    book.add_limit_order(OrderSide::BUY, 105.0, 8);

    CHECK(book.get_order(3)->status == OrderStatus::REJECTED);

    OrderBook rebuilt = replay("T", book.command_log().all(), 0.5);
    std::string err;
    CHECK(rebuilt.validate(&err));
    CHECK(rebuilt.tick_size() == 0.5);
    CHECK(same_depth(book.depth(50), rebuilt.depth(50)));
    CHECK(book.total_orders() == rebuilt.total_orders());
    // The refusal came back on replay rather than being quietly accepted.
    CHECK(rebuilt.get_order(3)->status == OrderStatus::REJECTED);
    CHECK(rebuilt.trades().size() == book.trades().size());
}

int main() {
    test_price_time_priority();
    test_stop_when_no_cross();
    test_market_order_never_rests();
    test_cancel_then_future_match_skips_cancelled();
    test_modify_increase_requeues_but_decrease_keeps_priority();
    test_modify_rejects_invalid_quantities();
    test_modify_price_moves_level_and_loses_priority();
    test_modify_price_that_crosses_matches_immediately();
    test_modify_cannot_resurrect_dead_orders();
    test_bbo_on_empty_book_is_empty_not_zero();
    test_depth_reports_price_order_and_aggregates();
    test_sequence_numbers_are_monotonic_and_unique();
    test_trades_recorded_and_events_ordered();
    test_trade_keeps_passive_order_price();
    test_spec_worked_example();
    test_multiple_matches_across_levels();
    test_price_time_priority_during_matching();
    test_sell_aggressor_sweeps_bids_best_first();
    test_trade_generation_and_journal();
    test_partial_fill_keeps_queue_position();
    test_market_order_sweeps_multiple_levels();
    test_zero_quantity_is_rejected_not_corrupting();
    test_order_status_through_lifecycle();
    test_lifecycle_transition_table();
    test_order_lifecycle_happy_paths();
    test_order_history_records_every_step();
    test_order_validation_rejects_bad_submissions();
    test_unique_order_ids_and_idempotent_retry();
    test_tick_size_validation();
    test_trade_history_queries();
    test_replay_preserves_tick_size_and_rejections();
    test_replay_reconstructs_identical_book();
    test_invariants_hold_under_random_stream();

    if (failures == 0) {
        std::cout << "All tests passed\n";
        return 0;
    }
    std::cout << failures << " test(s) failed\n";
    return 1;
}
