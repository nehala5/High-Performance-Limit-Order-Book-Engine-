#pragma once

#include "order.h"
#include "trade.h"
#include <variant>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <iomanip>

// Derived audit trail. Every field needed to order the events is present, but
// this log is an *output* of matching -- see command.h for the input stream
// that a book can be rebuilt from.

struct OrderAdded {
    OrderId  id;
    SeqNum   seq;
    OrderSide side;
    OrderType type;
    Price    price;
    Quantity qty;
    Timestamp timestamp;
};

struct OrderFilled {
    OrderId  id;
    SeqNum   seq;
    Quantity filled_qty;
    Quantity remaining_qty;
    Timestamp timestamp;
};

struct OrderCancelled {
    OrderId  id;
    SeqNum   seq;
    Quantity remaining_qty;
    Timestamp timestamp;
};

struct OrderModified {
    OrderId  id;
    SeqNum   seq;
    Quantity old_qty;
    Quantity new_qty;
    Price    old_price;
    Price    new_price;
    bool     price_changed;
    bool     priority_lost;
    Timestamp timestamp;
};

struct TradeEvent {
    Trade trade;
};

using Event = std::variant<OrderAdded, OrderFilled, OrderCancelled, OrderModified, TradeEvent>;

struct EventStream {
    std::vector<Event> events;

    void record(OrderAdded e)      { events.push_back(e); }
    void record(OrderFilled e)     { events.push_back(e); }
    void record(OrderCancelled e)  { events.push_back(e); }
    void record(OrderModified e)   { events.push_back(e); }
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
        os << "OrderAdded   seq=" << e.seq << " id=" << e.id
           << " " << (e.side == OrderSide::BUY ? "BUY" : "SELL")
           << " " << (e.type == OrderType::LIMIT ? "LIMIT" : "MARKET");
        if (e.type == OrderType::LIMIT)
            os << " px=" << std::fixed << std::setprecision(2) << e.price;
        os << " qty=" << e.qty;
    }
    static void format(std::ostream& os, const OrderFilled& e) {
        os << "OrderFilled  seq=" << e.seq << " id=" << e.id
           << " filled=" << e.filled_qty
           << " remaining=" << e.remaining_qty;
    }
    static void format(std::ostream& os, const OrderCancelled& e) {
        os << "OrderCancelled seq=" << e.seq << " id=" << e.id
           << " remaining=" << e.remaining_qty;
    }
    static void format(std::ostream& os, const OrderModified& e) {
        os << "OrderModified seq=" << e.seq << " id=" << e.id
           << " qty " << e.old_qty << "->" << e.new_qty;
        if (e.price_changed)
            os << " px " << std::fixed << std::setprecision(2) << e.old_price
               << "->" << e.new_price;
        os << (e.priority_lost ? " [requeued]" : " [priority kept]");
    }
    static void format(std::ostream& os, const TradeEvent& e) {
        os << "Trade        seq=" << e.trade.seq << " id=" << e.trade.trade_id
           << " aggressor=" << e.trade.aggressor_id
           << " passive=" << e.trade.passive_id
           << " " << (e.trade.side == OrderSide::BUY ? "BUY" : "SELL")
           << " px=" << std::fixed << std::setprecision(2) << e.trade.price
           << " qty=" << e.trade.qty;
    }
};
