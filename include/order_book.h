#pragma once

#include "order.h"
#include "trade.h"
#include "event.h"
#include "market_data.h"

#include <map>
#include <unordered_map>
#include <deque>
#include <vector>
#include <functional>
#include <string>

class OrderBook {
public:
    explicit OrderBook(std::string symbol);

    const std::string& symbol() const;

    OrderId add_limit_order(OrderSide side, Price price, Quantity qty);
    OrderId add_market_order(OrderSide side, Quantity qty);

    bool cancel_order(OrderId id);
    bool modify_order(OrderId id, Quantity new_qty);

    const Order* get_order(OrderId id) const;

    L2Snapshot snapshot(size_t depth = 10) const;
    const EventStream& event_stream() const;

    size_t bid_levels() const;
    size_t ask_levels() const;
    size_t total_orders() const;

    void print_book(size_t depth = 5) const;

private:
    struct PriceLevel {
        std::deque<OrderId> orders;
    };

    std::string symbol_;
    OrderId next_order_id_;
    uint64_t next_trade_id_;

    std::unordered_map<OrderId, Order> orders_;

    std::map<Price, PriceLevel, std::greater<Price>> bids_;
    std::map<Price, PriceLevel, std::less<Price>>    asks_;

    EventStream events_;
    std::vector<Trade> trades_;

    bool price_crosses(OrderSide side, Price incoming, Price resting) const;
    std::vector<Trade> match_order(Order& incoming);

    void add_to_book(Order& order);
    void remove_from_book(OrderId id);
    void update_order_status(Order& order, Quantity fill_qty);

    Price best_bid() const;
    Price best_ask() const;
};
