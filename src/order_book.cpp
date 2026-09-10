#include "order_book.h"
#include <iostream>
#include <algorithm>
#include <cassert>

OrderBook::OrderBook(std::string symbol)
    : symbol_(std::move(symbol))
    , next_order_id_(1)
    , next_trade_id_(1)
{}

const std::string& OrderBook::symbol() const { return symbol_; }

bool OrderBook::price_crosses(OrderSide side, Price incoming, Price resting) const {
    if (side == OrderSide::BUY)
        return incoming >= resting;
    else
        return incoming <= resting;
}

void OrderBook::add_to_book(Order& order) {
    auto& level = (order.side == OrderSide::BUY) ? bids_[order.price] : asks_[order.price];
    level.orders.push_back(order.id);
    if (order.filled_qty == 0)
        order.status = OrderStatus::PENDING;
}

void OrderBook::remove_from_book(OrderId id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return;
    auto& order = it->second;

    auto remove_from = [&](auto& book) {
        auto level_it = book.find(order.price);
        if (level_it == book.end()) return;
        auto& dq = level_it->second.orders;
        dq.erase(std::remove(dq.begin(), dq.end(), id), dq.end());
        if (dq.empty()) book.erase(level_it);
    };

    if (order.side == OrderSide::BUY)
        remove_from(bids_);
    else
        remove_from(asks_);
}

void OrderBook::update_order_status(Order& order, Quantity fill_qty) {
    order.filled_qty += fill_qty;
    if (order.is_fully_filled()) {
        order.status = OrderStatus::FILLED;
    } else {
        order.status = OrderStatus::PARTIAL;
    }
}

std::vector<Trade> OrderBook::match_order(Order& incoming) {
    std::vector<Trade> result;

    auto do_match = [&](auto& opposing) {

    while (!opposing.empty() && incoming.remaining() > 0) {
        auto level_it = opposing.begin();
        auto& level = level_it->second;

        bool can_cross = (incoming.type == OrderType::MARKET) ||
                         price_crosses(incoming.side, incoming.price, level_it->first);

        if (!can_cross) break;

        while (!level.orders.empty() && incoming.remaining() > 0) {
            OrderId resting_id = level.orders.front();
            auto resting_it = orders_.find(resting_id);
            if (resting_it == orders_.end()) {
                level.orders.pop_front();
                continue;
            }
            Order& resting = resting_it->second;

            Quantity fill = std::min(incoming.remaining(), resting.remaining());

            update_order_status(incoming, fill);
            update_order_status(resting, fill);

            Trade trade{
                next_trade_id_++,
                incoming.id,
                resting.id,
                incoming.side,
                resting.price,
                fill,
                Timestamp::clock::now()
            };
            result.push_back(trade);
            trades_.push_back(trade);
            events_.record(TradeEvent{trade});

            if (resting.is_fully_filled()) {
                events_.record(OrderFilled{resting.id, resting.filled_qty, 0, Timestamp::clock::now()});
                level.orders.pop_front();
            }
        }

        if (level.orders.empty()) {
            opposing.erase(level_it);
        }
    }
    };

    if (incoming.side == OrderSide::BUY)
        do_match(asks_);
    else
        do_match(bids_);

    return result;
}

OrderId OrderBook::add_limit_order(OrderSide side, Price price, Quantity qty) {
    Order order{
        next_order_id_++,
        side,
        OrderType::LIMIT,
        OrderStatus::PENDING,
        price,
        qty,
        0,
        Timestamp::clock::now()
    };

    events_.record(OrderAdded{order.id, side, OrderType::LIMIT, price, qty, order.timestamp});

    auto trades = match_order(order);

    if (!order.is_fully_filled()) {
        add_to_book(order);
    } else {
        events_.record(OrderFilled{order.id, order.filled_qty, 0, order.timestamp});
    }

    orders_.emplace(order.id, order);
    return order.id;
}

OrderId OrderBook::add_market_order(OrderSide side, Quantity qty) {
    Price dummy_price = (side == OrderSide::BUY) ? 1e18 : 0.0;

    Order order{
        next_order_id_++,
        side,
        OrderType::MARKET,
        OrderStatus::PENDING,
        dummy_price,
        qty,
        0,
        Timestamp::clock::now()
    };

    events_.record(OrderAdded{order.id, side, OrderType::MARKET, dummy_price, qty, order.timestamp});

    auto trades = match_order(order);

    if (!order.is_fully_filled()) {
        events_.record(OrderFilled{order.id, order.filled_qty, order.remaining(), order.timestamp});
    } else {
        events_.record(OrderFilled{order.id, order.filled_qty, 0, order.timestamp});
    }

    orders_.emplace(order.id, order);
    return order.id;
}

bool OrderBook::cancel_order(OrderId id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return false;
    Order& order = it->second;
    if (order.status == OrderStatus::FILLED || order.status == OrderStatus::CANCELLED)
        return false;

    Quantity remaining = order.remaining();
    remove_from_book(id);
    order.status = OrderStatus::CANCELLED;
    events_.record(OrderCancelled{id, remaining, Timestamp::clock::now()});
    return true;
}

bool OrderBook::modify_order(OrderId id, Quantity new_qty) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return false;
    Order& order = it->second;
    if (order.status == OrderStatus::FILLED || order.status == OrderStatus::CANCELLED)
        return false;
    if (order.type != OrderType::LIMIT)
        return false;

    if (new_qty <= order.filled_qty) return false;

    remove_from_book(id);

    order.qty = new_qty;
    if (order.filled_qty == 0)
        order.status = OrderStatus::PENDING;
    else
        order.status = OrderStatus::PARTIAL;

    add_to_book(order);

    events_.record(OrderAdded{order.id, order.side, order.type, order.price, order.qty, order.timestamp});
    return true;
}

const Order* OrderBook::get_order(OrderId id) const {
    auto it = orders_.find(id);
    return (it != orders_.end()) ? &it->second : nullptr;
}

Price OrderBook::best_bid() const {
    return bids_.empty() ? 0.0 : bids_.begin()->first;
}

Price OrderBook::best_ask() const {
    return asks_.empty() ? 0.0 : asks_.begin()->first;
}

L2Snapshot OrderBook::snapshot(size_t depth) const {
    L2Snapshot snap;

    size_t i = 0;
    for (auto it = bids_.begin(); it != bids_.end() && i < depth; ++it, ++i) {
        snap.bids.push_back({it->first, 0, it->second.orders.size()});
        for (OrderId oid : it->second.orders) {
            auto oit = orders_.find(oid);
            if (oit != orders_.end()) snap.bids.back().total_qty += oit->second.remaining();
        }
    }

    i = 0;
    for (auto it = asks_.begin(); it != asks_.end() && i < depth; ++it, ++i) {
        snap.asks.push_back({it->first, 0, it->second.orders.size()});
        for (OrderId oid : it->second.orders) {
            auto oit = orders_.find(oid);
            if (oit != orders_.end()) snap.asks.back().total_qty += oit->second.remaining();
        }
    }

    if (!snap.bids.empty()) snap.best_bid = snap.bids.front().price;
    if (!snap.asks.empty()) snap.best_ask = snap.asks.front().price;
    if (snap.best_bid && snap.best_ask) {
        snap.mid_price = (*snap.best_bid + *snap.best_ask) / 2.0;
        snap.spread = *snap.best_ask - *snap.best_bid;
    }

    return snap;
}

const EventStream& OrderBook::event_stream() const { return events_; }

size_t OrderBook::bid_levels() const { return bids_.size(); }
size_t OrderBook::ask_levels() const { return asks_.size(); }
size_t OrderBook::total_orders() const { return orders_.size(); }

void OrderBook::print_book(size_t depth) const {
    auto snap = snapshot(depth);
    snap.print();
}
