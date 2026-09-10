#include "order_book.h"
#include <cassert>
#include <iostream>

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << #cond << " @line " << __LINE__ << "\n"; \
        ++failures; \
    } \
} while (0)

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
}

void test_modify_decrease_keeps_remaining() {
    OrderBook book("T");
    OrderId b = book.add_limit_order(OrderSide::BUY, 100, 10);

    CHECK(book.modify_order(b, 30));
    const Order* o = book.get_order(b);
    CHECK(o->qty == 30);
    CHECK(o->status == OrderStatus::PENDING);
    CHECK(o->remaining() == 30);

    CHECK(!book.modify_order(b, 0));
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

int main() {
    test_price_time_priority();
    test_stop_when_no_cross();
    test_market_order_never_rests();
    test_cancel_then_future_match_skips_cancelled();
    test_modify_decrease_keeps_remaining();
    test_trades_recorded_and_events_ordered();

    if (failures == 0) {
        std::cout << "All tests passed\n";
        return 0;
    }
    std::cout << failures << " test(s) failed\n";
    return 1;
}