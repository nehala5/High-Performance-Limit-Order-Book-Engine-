#include "order_book.h"

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
    OrderId b1 = book.add_limit_order(OrderSide::BUY, 100, 5);
    OrderId b2 = book.add_limit_order(OrderSide::BUY, 100, 7);
    OrderId b3 = book.add_limit_order(OrderSide::BUY, 99, 3);

    book.add_limit_order(OrderSide::SELL, 100, 6);

    const Order* o1 = book.get_order(b1);
    const Order* o2 = book.get_order(b2);
    const Order* o3 = book.get_order(b3);

    CHECK(o1->status == OrderStatus::FILLED);
    CHECK(o1->filled_qty == 5);
    CHECK(o2->status == OrderStatus::PARTIAL);
    CHECK(o2->filled_qty == 1);
    CHECK(o2->remaining() == 6);
    CHECK(o3->status == OrderStatus::PENDING);
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

    OrderId m = book.add_market_order(OrderSide::BUY, 20);
    const Order* mo = book.get_order(m);

    CHECK(mo->status == OrderStatus::FILLED || mo->status == OrderStatus::PARTIAL);
    CHECK(mo->filled_qty == 9);
    CHECK(mo->remaining() == 11);
    CHECK(book.ask_levels() == 0);
    CHECK(!mo->is_resting());
    CHECK_VALID(book);
}

void test_cancel_then_future_match_skips_cancelled() {
    OrderBook book("T");
    OrderId b1 = book.add_limit_order(OrderSide::BUY, 100, 10);
    OrderId b2 = book.add_limit_order(OrderSide::BUY, 100, 10);

    CHECK(book.cancel_order(b1));
    const Order* o = book.get_order(b1);
    CHECK(o->status == OrderStatus::CANCELLED);

    book.add_limit_order(OrderSide::SELL, 100, 6);

    const Order* r = book.get_order(b2);
    CHECK(r->status == OrderStatus::PARTIAL);
    CHECK(r->filled_qty == 6);
    CHECK(r->remaining() == 4);
    CHECK_VALID(book);
}

void test_modify_increase_requeues_but_decrease_keeps_priority() {
    OrderBook book("T");
    OrderId a = book.add_limit_order(OrderSide::BUY, 100, 10);
    OrderId b = book.add_limit_order(OrderSide::BUY, 100, 10);
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
    OrderId b = book.add_limit_order(OrderSide::BUY, 100, 10);
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
    OrderId a = book.add_limit_order(OrderSide::BUY, 100, 10);
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
    OrderId lone = book.add_limit_order(OrderSide::SELL, 200, 5);
    CHECK(book.modify_order(lone, 5, 201.0));
    CHECK(book.best_ask() == 201.0);
    CHECK(!book.best_bid() || *book.best_bid() < 201.0);
    CHECK_VALID(book);
}

void test_modify_cannot_resurrect_dead_orders() {
    OrderBook book("T");
    OrderId a = book.add_limit_order(OrderSide::BUY, 100, 10);
    CHECK(book.cancel_order(a));
    CHECK(!book.modify_order(a, 20));

    OrderId f = book.add_limit_order(OrderSide::BUY, 100, 5);
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

    // A real bid at 0.0 must be distinguishable from no bid at all.
    book.add_limit_order(OrderSide::BUY, 0.0, 10);
    CHECK(book.best_bid().has_value());
    CHECK(*book.best_bid() == 0.0);
    CHECK(!book.best_ask().has_value());
    CHECK(!book.mid_price().has_value());   // one-sided book has no mid
    CHECK(!book.spread().has_value());
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
    OrderId a = book.add_limit_order(OrderSide::BUY, 100, 10);
    OrderId b = book.add_limit_order(OrderSide::SELL, 105, 10);
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

// The stated goal: a stream of orders must always rebuild the same book.
void test_replay_reconstructs_identical_book() {
    OrderBook book("T");

    OrderId a = book.add_limit_order(OrderSide::BUY, 100, 10);
    OrderId b = book.add_limit_order(OrderSide::BUY, 100, 10);
    OrderId c = book.add_limit_order(OrderSide::BUY,  99, 10);
    OrderId d = book.add_limit_order(OrderSide::SELL, 101, 10);
    OrderId e = book.add_limit_order(OrderSide::SELL, 102, 10);
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
            Price px = 95.0 + static_cast<double>(next() % 11);   // 95..105
            Quantity qty = 1 + next() % 20;
            OrderId id = book.add_limit_order(side, px, qty);
            if (book.get_order(id)->is_active()) live.push_back(id);
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
            OrderId id = book.add_market_order(side, 1 + next() % 25);
            if (book.get_order(id)->is_active()) live.push_back(id);
        } else {
            OrderId id = live[next() % live.size()];
            Price px = 95.0 + static_cast<double>(next() % 11);
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
    OrderId b = book.add_limit_order(OrderSide::BUY, 90, 10);
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
    CHECK(o->status == OrderStatus::PARTIAL);
    CHECK(!book.best_ask().has_value() || *book.best_ask() > 101.0);
    CHECK(*book.best_bid() == 101.0);
    CHECK(book.ask_levels() == 0);
    CHECK(book.bid_levels() == 1);
    CHECK(o->is_resting());
    CHECK(book.queue(OrderSide::BUY, 101.0).size() == 1);
    CHECK_VALID(book);

    // Repositioning far enough to fill completely leaves nothing resting.
    OrderId c = book.add_limit_order(OrderSide::BUY, 90, 5);
    book.add_limit_order(OrderSide::SELL, 102, 5);
    CHECK(book.modify_order(c, 5, 102.0));
    CHECK(book.get_order(c)->status == OrderStatus::FILLED);
    CHECK(!book.get_order(c)->is_resting());
    CHECK(book.queue(OrderSide::BUY, 102.0).empty());
    CHECK(book.bid_levels() == 1);   // only the earlier bid remains
    CHECK_VALID(book);
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
    test_replay_reconstructs_identical_book();
    test_invariants_hold_under_random_stream();

    if (failures == 0) {
        std::cout << "All tests passed\n";
        return 0;
    }
    std::cout << failures << " test(s) failed\n";
    return 1;
}
