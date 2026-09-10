#pragma once

#include "order.h"
#include "trade.h"
#include <variant>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <iomanip>

struct OrderAdded {
    OrderId  id;
    OrderSide side;
    OrderType type;
    Price    price;
    Quantity qty;
    Timestamp timestamp;
};

struct OrderFilled {
    OrderId  id;
    Quantity filled_qty;
    Quantity remaining_qty;
    Timestamp timestamp;
};

struct OrderCancelled {
    OrderId  id;
    Quantity remaining_qty;
    Timestamp timestamp;
};

struct TradeEvent {
    Trade trade;
};

using Event = std::variant<OrderAdded, OrderFilled, OrderCancelled, TradeEvent>;

struct EventStream {
    std::vector<Event> events;

    void record(OrderAdded e)      { events.push_back(e); }
    void record(OrderFilled e)     { events.push_back(e); }
    void record(OrderCancelled e)  { events.push_back(e); }
    void record(TradeEvent e)      { events.push_back(e); }

    const std::vector<Event>& all() const { return events; }
    size_t size() const { return events.size(); }
    void clear() { events.clear(); }

    std::string to_string(size_t idx, const Event& e) const {
        std::ostringstream oss;
        oss << "[" << idx << "] ";
        std::visit([&](auto& arg) { format(oss, arg); }, e);
        return oss.str();
    }

    void print() const {
        for (size_t i = 0; i < events.size(); ++i) {
            std::cout << to_string(i, events[i]) << "\n";
        }
    }

private:
    static void format(std::ostream& os, const OrderAdded& e) {
        os << "OrderAdded   id=" << e.id
           << " " << (e.side == OrderSide::BUY ? "BUY" : "SELL")
           << " " << (e.type == OrderType::LIMIT ? "LIMIT" : "MARKET")
           << " px=" << std::fixed << std::setprecision(2) << e.price
           << " qty=" << e.qty;
    }
    static void format(std::ostream& os, const OrderFilled& e) {
        os << "OrderFilled  id=" << e.id
           << " filled=" << e.filled_qty
           << " remaining=" << e.remaining_qty;
    }
    static void format(std::ostream& os, const OrderCancelled& e) {
        os << "OrderCancelled id=" << e.id
           << " remaining=" << e.remaining_qty;
    }
    static void format(std::ostream& os, const TradeEvent& e) {
        os << "Trade        id=" << e.trade.trade_id
           << " aggressor=" << e.trade.aggressor_id
           << " passive=" << e.trade.passive_id
           << " " << (e.trade.side == OrderSide::BUY ? "BUY" : "SELL")
           << " px=" << std::fixed << std::setprecision(2) << e.trade.price
           << " qty=" << e.trade.qty;
    }
};
