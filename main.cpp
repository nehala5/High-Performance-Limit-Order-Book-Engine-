#include "order_book.h"
#include <iostream>

int main() {
    OrderBook book("BTC/USD");

    std::cout << "=== 1. Populate Order Book ===\n";
    book.add_limit_order(OrderSide::BUY,  100.00, 10);
    book.add_limit_order(OrderSide::BUY,  99.50,  20);
    book.add_limit_order(OrderSide::BUY,  99.00,  15);
    book.add_limit_order(OrderSide::SELL, 100.50, 12);
    book.add_limit_order(OrderSide::SELL, 101.00, 8);
    book.add_limit_order(OrderSide::SELL, 101.50, 25);

    std::cout << "\n--- Initial Book ---\n";
    book.print_book();

    std::cout << "\n=== 2. Aggressive Buy crosses two ask levels ===\n";
    book.add_limit_order(OrderSide::BUY, 101.00, 15);

    std::cout << "\n--- After crossing buy ---\n";
    book.print_book();

    std::cout << "\n=== 3. Partial Fill (buy 50 @ 101.00 but only 8 resting) ===\n";
    OrderId partial_id = book.add_limit_order(OrderSide::BUY, 101.00, 50);

    std::cout << "\n--- After partial fill ---\n";
    book.print_book();

    const Order* op = book.get_order(partial_id);
    if (op) std::cout << "Order " << partial_id << " status: " << op->status_str()
                      << " filled=" << op->filled_qty << "/" << op->qty << "\n";

    std::cout << "\n=== 4. Cancel Order (order 3, resting BUY 99.00) ===\n";
    bool cancelled = book.cancel_order(3);
    std::cout << "Cancel returned: " << (cancelled ? "true" : "false") << "\n";
    const Order* c3 = book.get_order(3);
    if (c3) std::cout << "Order 3 status: " << c3->status_str() << "\n";

    std::cout << "\n--- After cancel ---\n";
    book.print_book();

    std::cout << "\n=== 5. Modify Order (order 2, BUY 99.50, increase 20 -> 35, loses priority) ===\n";
    bool modified = book.modify_order(2, 35);
    std::cout << "Modify returned: " << (modified ? "true" : "false") << "\n";
    const Order* m2 = book.get_order(2);
    if (m2) std::cout << "Order 2 status: " << m2->status_str()
                      << " qty=" << m2->qty << " filled=" << m2->filled_qty << "\n";

    std::cout << "\n--- After modify ---\n";
    book.print_book();

    std::cout << "\n=== 6. Market Order (buy 30) ===\n";
    book.add_market_order(OrderSide::BUY, 30);

    std::cout << "\n--- After market buy ---\n";
    book.print_book();

    std::cout << "\n=== 7. L2 Snapshot (depth=3) ===\n";
    auto snap = book.snapshot(3);
    snap.print();

    std::cout << "\n=== 8. Event Stream (audit trail) ===\n";
    book.event_stream().print();

    std::cout << "\nTotal orders in book: " << book.total_orders() << "\n";
    std::cout << "Bid levels: " << book.bid_levels() << " Ask levels: " << book.ask_levels() << "\n";

    return 0;
}
