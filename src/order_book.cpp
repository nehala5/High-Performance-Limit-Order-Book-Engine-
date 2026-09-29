#include "order_book.h"

#include <algorithm>
#include <cmath>
#include <iostream>

const std::vector<OrderTransition> OrderBook::empty_history_;

OrderBook::OrderBook(std::string symbol, Price tick_size)
    : symbol_(std::move(symbol))
    , tick_size_(tick_size)
    , next_order_id_(1)
    , next_trade_id_(1)
    , next_seq_(0)
{}

const std::string& OrderBook::symbol() const { return symbol_; }
Price OrderBook::tick_size() const { return tick_size_; }

SeqNum OrderBook::sequence() const { return next_seq_; }

bool OrderBook::price_crosses(OrderSide side, Price incoming, Price resting) const {
    if (side == OrderSide::BUY)
        return incoming >= resting;
    else
        return incoming <= resting;
}

bool OrderBook::price_is_valid(Price price, RejectReason& why) const {
    // A non-finite price must never reach the book: std::map<double> with a
    // NaN key breaks its own comparator's strict weak ordering, which quietly
    // corrupts level iteration and depth.
    if (!std::isfinite(price)) { why = RejectReason::PRICE_NOT_FINITE; return false; }
    if (price <= 0.0)           { why = RejectReason::PRICE_NOT_POSITIVE; return false; }
    if (tick_size_ > 0.0) {
        double ticks = price / tick_size_;
        if (std::abs(ticks - std::round(ticks)) > 1e-9) {
            why = RejectReason::OFF_TICK;
            return false;
        }
    }
    return true;
}

bool OrderBook::set_status(Order& order, OrderStatus to, SeqNum seq) {
    if (order.status == to) return true;
    if (!can_transition_to(order.status, to)) return false;

    OrderStatus from = order.status;
    order.status = to;
    order.seq = seq;
    history_[order.id].push_back(OrderTransition{order.id, seq, from, to, Timestamp::clock::now()});
    return true;
}

void OrderBook::add_to_book(Order& order, SeqNum seq) {
    auto push = [](auto& book, Price price, OrderId id) {
        book[price].orders.push_back(id);
    };
    if (order.side == OrderSide::BUY)
        push(bids_, order.price, order.id);
    else
        push(asks_, order.price, order.id);

    // An order that matched on arrival is already past OPEN.
    OrderStatus want = (order.filled_qty == 0) ? OrderStatus::OPEN
                                               : OrderStatus::PARTIALLY_FILLED;
    set_status(order, want, seq);
}

void OrderBook::remove_from_book(OrderId id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return;
    const Order& order = it->second;

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

void OrderBook::requeue_in_book(OrderId id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return;
    Order& order = it->second;

    // Drops the order out of its slot and appends it to the back of the same
    // level, so it keeps its price but loses its place in the queue.
    auto move_to_back = [](auto& book, Price price, OrderId oid) {
        auto level_it = book.find(price);
        if (level_it == book.end()) {
            book[price].orders.push_back(oid);
            return;
        }
        auto& dq = level_it->second.orders;
        dq.erase(std::remove(dq.begin(), dq.end(), oid), dq.end());
        dq.push_back(oid);
    };

    if (order.side == OrderSide::BUY)
        move_to_back(bids_, order.price, id);
    else
        move_to_back(asks_, order.price, id);
}

void OrderBook::update_order_status(Order& order, Quantity fill_qty, SeqNum seq) {
    order.filled_qty += fill_qty;
    set_status(order,
               order.is_fully_filled() ? OrderStatus::FILLED : OrderStatus::PARTIALLY_FILLED,
               seq);
}

std::vector<Trade> OrderBook::match_order(Order& incoming, SeqNum seq) {
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
                // A stale entry would mean the queue disagrees with the order
                // set. validate() says it cannot happen, but matching must
                // never trade against a dead order, so drop it and move on.
                if (resting_it == orders_.end() || !resting_it->second.is_resting()) {
                    level.orders.pop_front();
                    continue;
                }
                Order& resting = resting_it->second;

                Quantity fill = std::min(incoming.remaining(), resting.remaining());

                update_order_status(incoming, fill, seq);
                update_order_status(resting, fill, seq);

                // A trade is stamped with the sequence number of the request
                // that produced it, not with a fresh one.
                Trade trade{
                    next_trade_id_++,
                    seq,
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
                    level.orders.pop_front();
                    events_.record(OrderFilled{resting.id, seq, resting.filled_qty,
                                               resting.remaining(), Timestamp::clock::now()});
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

ExecutionReport OrderBook::reject_order(OrderId id, SeqNum seq, OrderSide side, OrderType type,
                                        Price price, Quantity qty, RejectReason why,
                                        Timestamp ts) {
    orders_.emplace(id, Order{id, seq, side, type, OrderStatus::NEW, price, qty, 0, ts});
    // Rejection is a step in the life like any other, so the order's history
    // still explains how it ended up here.
    set_status(orders_.at(id), OrderStatus::REJECTED, seq);
    events_.record(OrderRejected{id, seq, side, type, price, qty, reason_name(why), ts});

    if (type == OrderType::LIMIT) {
        commands_.record(OrderCommand{CommandKind::ADD_LIMIT, side, price, qty, id, 0, 0.0, false, seq, ts});
    } else {
        commands_.record(OrderCommand{CommandKind::ADD_MARKET, side, price, qty, id, 0, 0.0, false, seq, ts});
    }

    ExecutionReport report;
    report.order_id      = id;
    report.seq           = seq;
    report.type          = type;
    report.status        = OrderStatus::REJECTED;
    report.requested_qty = qty;
    report.filled_qty    = 0;
    report.leaves_qty    = 0;
    report.reject_code   = why;
    return report;
}

ExecutionReport OrderBook::add_limit_order(OrderSide side, Price price, Quantity qty,
                                           OrderId requested_id) {
    OrderId id = (requested_id != 0) ? requested_id : next_order_id_++;
    SeqNum seq = ++next_seq_;
    Timestamp ts = Timestamp::clock::now();

    if (orders_.count(id) != 0) {
        return reject_order(id, seq, side, OrderType::LIMIT, price, qty,
                            RejectReason::DUPLICATE_ORDER_ID, ts);
    }
    if (qty == 0) {
        return reject_order(id, seq, side, OrderType::LIMIT, price, qty,
                            RejectReason::QUANTITY_NOT_POSITIVE, ts);
    }
    RejectReason why = RejectReason::NONE;
    if (!price_is_valid(price, why)) {
        return reject_order(id, seq, side, OrderType::LIMIT, price, qty, why, ts);
    }

    if (requested_id != 0 && id >= next_order_id_) next_order_id_ = id + 1;

    // NEW until it either starts working or trades.
    orders_.emplace(id, Order{id, seq, side, OrderType::LIMIT, OrderStatus::NEW,
                              price, qty, 0, ts});
    Order& order = orders_.at(id);

    commands_.record(OrderCommand{CommandKind::ADD_LIMIT, side, price, qty, id, 0, 0.0, false, seq, ts});
    events_.record(OrderAdded{id, seq, side, OrderType::LIMIT, price, qty, ts});

    std::vector<Trade> executed = match_order(order, seq);

    if (order.remaining() > 0) {
        add_to_book(order, seq);
    }
    if (order.filled_qty > 0) {
        events_.record(OrderFilled{id, seq, order.filled_qty, order.remaining(),
                                   Timestamp::clock::now()});
    }

    ExecutionReport report;
    report.order_id      = id;
    report.seq           = seq;
    report.type          = OrderType::LIMIT;
    report.status        = order.status;
    report.requested_qty = qty;
    report.filled_qty    = order.filled_qty;
    report.leaves_qty    = order.remaining();
    for (const auto& t : executed) report.fills.push_back(Fill{t.passive_id, t.price, t.qty});
    return report;
}

ExecutionReport OrderBook::add_market_order(OrderSide side, Quantity qty, OrderId requested_id) {
    OrderId id = (requested_id != 0) ? requested_id : next_order_id_++;
    SeqNum seq = ++next_seq_;
    Timestamp ts = Timestamp::clock::now();

    // Market orders never rest, so the price is only a placeholder used by the
    // crossing test.
    Price placeholder = (side == OrderSide::BUY) ? 1e18 : 0.0;

    if (orders_.count(id) != 0) {
        return reject_order(id, seq, side, OrderType::MARKET, placeholder, qty,
                            RejectReason::DUPLICATE_ORDER_ID, ts);
    }
    if (qty == 0) {
        return reject_order(id, seq, side, OrderType::MARKET, placeholder, qty,
                            RejectReason::QUANTITY_NOT_POSITIVE, ts);
    }

    if (requested_id != 0 && id >= next_order_id_) next_order_id_ = id + 1;

    orders_.emplace(id, Order{id, seq, side, OrderType::MARKET, OrderStatus::NEW,
                              placeholder, qty, 0, ts});
    Order& order = orders_.at(id);

    commands_.record(OrderCommand{CommandKind::ADD_MARKET, side, placeholder, qty, id, 0, 0.0, false, seq, ts});
    events_.record(OrderAdded{id, seq, side, OrderType::MARKET, placeholder, qty, ts});

    std::vector<Trade> executed = match_order(order, seq);

    // Whatever is left is simply discarded -- a market order has no resting
    // remainder to work with.
    if (order.filled_qty > 0) {
        events_.record(OrderFilled{id, seq, order.filled_qty, order.remaining(),
                                   Timestamp::clock::now()});
    }

    ExecutionReport report;
    report.order_id      = id;
    report.seq           = seq;
    report.type          = OrderType::MARKET;
    report.status        = order.status;
    report.requested_qty = qty;
    report.filled_qty    = order.filled_qty;
    report.leaves_qty    = order.remaining();
    for (const auto& t : executed) report.fills.push_back(Fill{t.passive_id, t.price, t.qty});
    return report;
}

bool OrderBook::cancel_order(OrderId id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return false;
    Order& order = it->second;
    if (!order.is_working()) return false;

    SeqNum seq = ++next_seq_;
    Timestamp ts = Timestamp::clock::now();

    commands_.record(OrderCommand{CommandKind::CANCEL, order.side, order.price, 0, id, 0, 0.0, false, seq, ts});

    Quantity remaining = order.remaining();
    remove_from_book(id);
    set_status(order, OrderStatus::CANCELLED, seq);
    events_.record(OrderCancelled{id, seq, remaining, ts});
    return true;
}

bool OrderBook::modify_order(OrderId id, Quantity new_qty, std::optional<Price> new_price) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return false;
    Order& order = it->second;
    if (!order.is_working()) return false;
    if (order.type != OrderType::LIMIT) return false;
    if (new_qty <= order.filled_qty) return false;
    if (new_price.has_value()) {
        RejectReason why = RejectReason::NONE;
        if (!price_is_valid(*new_price, why)) return false;
    }

    const Quantity old_qty = order.qty;
    const Price old_price = order.price;
    const bool price_changed = new_price.has_value() && *new_price != order.price;
    // Shrinking at an unchanged price leaves the queue untouched: an order
    // cannot improve its place in line by asking for less size.
    const bool priority_lost = price_changed || (new_qty > order.qty);

    SeqNum seq = ++next_seq_;
    Timestamp ts = Timestamp::clock::now();

    commands_.record(OrderCommand{CommandKind::MODIFY, order.side, order.price, 0, id, new_qty,
                                  new_price.value_or(order.price), new_price.has_value(),
                                  seq, ts});

    order.qty = new_qty;
    if (order.filled_qty == 0) set_status(order, OrderStatus::OPEN, seq);

    if (price_changed) {
        remove_from_book(id);   // must run while order.price is still the old one
        order.price = *new_price;
        // A new price makes this a new order as far as the opposing side is
        // concerned, so it has to trade before it is allowed to rest. Without
        // this the book could be left crossed.
        match_order(order, seq);
        if (order.remaining() > 0) {
            add_to_book(order, seq);
        }
    } else if (priority_lost) {
        requeue_in_book(id);
    }

    if (price_changed && order.filled_qty > 0) {
        events_.record(OrderFilled{id, seq, order.filled_qty, order.remaining(),
                                   Timestamp::clock::now()});
    }

    events_.record(OrderModified{id, seq, old_qty, new_qty, old_price, order.price,
                                 price_changed, priority_lost, ts});
    return true;
}

const Order* OrderBook::get_order(OrderId id) const {
    auto it = orders_.find(id);
    return (it != orders_.end()) ? &it->second : nullptr;
}

std::vector<OrderTransition> const& OrderBook::order_history(OrderId id) const {
    auto it = history_.find(id);
    return (it != history_.end()) ? it->second : empty_history_;
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::optional<Price> OrderBook::mid_price() const {
    auto b = best_bid();
    auto a = best_ask();
    if (!b || !a) return std::nullopt;
    return (*b + *a) / 2.0;
}

std::optional<Price> OrderBook::spread() const {
    auto b = best_bid();
    auto a = best_ask();
    if (!b || !a) return std::nullopt;
    return *a - *b;
}

Quantity OrderBook::level_qty(OrderSide side, Price price) const {
    // The two sides are distinct map types, so sum them separately.
    auto sum = [this, price](const auto& book) -> Quantity {
        auto it = book.find(price);
        if (it == book.end()) return 0;
        Quantity q = 0;
        for (OrderId oid : it->second.orders) {
            auto oit = orders_.find(oid);
            if (oit != orders_.end()) q += oit->second.remaining();
        }
        return q;
    };
    return (side == OrderSide::BUY) ? sum(bids_) : sum(asks_);
}

std::vector<OrderId> OrderBook::queue(OrderSide side, Price price) const {
    std::vector<OrderId> ids;
    auto collect = [price, &ids](const auto& book) {
        auto it = book.find(price);
        if (it == book.end()) return;
        for (OrderId id : it->second.orders) ids.push_back(id);
    };
    if (side == OrderSide::BUY) collect(bids_); else collect(asks_);
    return ids;
}

L2Snapshot OrderBook::depth(size_t levels) const {
    L2Snapshot snap;

    for (auto it = bids_.begin(); it != bids_.end() && snap.bids.size() < levels; ++it) {
        PriceLevel lvl{it->first, 0, it->second.orders.size()};
        for (OrderId oid : it->second.orders) {
            auto oit = orders_.find(oid);
            if (oit != orders_.end()) lvl.total_qty += oit->second.remaining();
        }
        snap.bids.push_back(lvl);
    }

    for (auto it = asks_.begin(); it != asks_.end() && snap.asks.size() < levels; ++it) {
        PriceLevel lvl{it->first, 0, it->second.orders.size()};
        for (OrderId oid : it->second.orders) {
            auto oit = orders_.find(oid);
            if (oit != orders_.end()) lvl.total_qty += oit->second.remaining();
        }
        snap.asks.push_back(lvl);
    }

    snap.best_bid  = best_bid();
    snap.best_ask  = best_ask();
    snap.mid_price = mid_price();
    snap.spread    = spread();

    return snap;
}

const EventStream& OrderBook::event_stream() const { return events_; }
const CommandLog&  OrderBook::command_log() const { return commands_; }

const std::vector<Trade>& OrderBook::trades() const { return trades_; }

std::vector<Trade> OrderBook::trades_since(SeqNum seq) const {
    std::vector<Trade> out;
    for (const auto& t : trades_) {
        if (t.seq > seq) out.push_back(t);
    }
    return out;
}

std::vector<Trade> OrderBook::trades_for_order(OrderId id) const {
    std::vector<Trade> out;
    for (const auto& t : trades_) {
        if (t.aggressor_id == id || t.passive_id == id) out.push_back(t);
    }
    return out;
}

Quantity OrderBook::total_volume() const {
    Quantity v = 0;
    for (const auto& t : trades_) v += t.qty;
    return v;
}

double OrderBook::total_notional() const {
    double n = 0.0;
    for (const auto& t : trades_) n += t.price * static_cast<double>(t.qty);
    return n;
}

size_t OrderBook::bid_levels() const { return bids_.size(); }
size_t OrderBook::ask_levels() const { return asks_.size(); }
size_t OrderBook::total_orders() const { return orders_.size(); }

bool OrderBook::validate(std::string* err) const {
    auto fail = [err](const std::string& msg) {
        if (err) *err = msg;
        return false;
    };
    auto oid = [](OrderId id) { return "order " + std::to_string(id); };

    for (const auto& [px, lvl] : bids_) {
        if (lvl.orders.empty()) return fail("empty bid level at " + std::to_string(px));
    }
    for (const auto& [px, lvl] : asks_) {
        if (lvl.orders.empty()) return fail("empty ask level at " + std::to_string(px));
    }

    for (const auto& [id, o] : orders_) {
        if (o.id != id) return fail(oid(id) + " stored under the wrong key");
        if (o.filled_qty > o.qty) return fail(oid(id) + " is overfilled");
        if (o.seq == 0) return fail(oid(id) + " has no sequence number");
        // A refused order keeps the price that was asked for, so only an
        // accepted order is held to the price invariant.
        if (o.is_accepted() && !o.price_is_sane())
            return fail(oid(id) + " has an unusable price");
        if (o.status != OrderStatus::REJECTED && o.is_fully_filled() && o.status != OrderStatus::FILLED)
            return fail(oid(id) + " is fully filled but reports " + o.status_str());
        if (o.is_resting() && o.remaining() == 0)
            return fail(oid(id) + " is marked resting with nothing left");
        // NEW and REJECTED must never be working, and a working order must
        // have come off NEW at some point.
        if (o.status == OrderStatus::OPEN && o.filled_qty > 0)
            return fail(oid(id) + " reports OPEN despite having filled");
        if (o.is_terminal() && o.is_resting())
            return fail(oid(id) + " is terminal yet still on the book");
        if (o.type == OrderType::MARKET && o.is_resting())
            return fail(oid(id) + " is a market order yet resting");

        // The recorded life must agree with the status the order reports now.
        auto hit = history_.find(id);
        if (hit != history_.end() && !hit->second.empty()) {
            const auto& steps = hit->second;
            if (steps.back().to != o.status)
                return fail(oid(id) + " reports " + o.status_str() +
                            " but its last recorded step was " + status_name(steps.back().to));
            for (size_t i = 1; i < steps.size(); ++i) {
                if (steps[i].from != steps[i - 1].to)
                    return fail(oid(id) + " has a broken transition chain");
                if (!can_transition_to(steps[i].from, steps[i].to))
                    return fail(oid(id) + " recorded an illegal transition");
            }
        } else if (o.status != OrderStatus::NEW) {
            return fail(oid(id) + " reports " + o.status_str() + " with no recorded history");
        }
    }

    std::unordered_map<OrderId, int> queued;
    auto scan = [&](const auto& book, OrderSide side) -> bool {
        for (const auto& [px, lvl] : book) {
            for (OrderId id : lvl.orders) {
                auto it = orders_.find(id);
                if (it == orders_.end()) return fail("queued " + oid(id) + " does not exist");
                const Order& o = it->second;
                if (o.side != side) return fail(oid(id) + " is queued on the wrong side");
                if (o.price != px) return fail(oid(id) + " is queued at the wrong price");
                if (!o.is_resting()) return fail(oid(id) + " is queued but is not resting");
                if (++queued[id] > 1) return fail(oid(id) + " is queued more than once");
            }
        }
        return true;
    };

    if (!scan(bids_, OrderSide::BUY)) return false;
    if (!scan(asks_, OrderSide::SELL)) return false;

    for (const auto& [id, o] : orders_) {
        if (o.is_resting() && queued[id] != 1)
            return fail(oid(id) + " should be on the book but is not");
    }

    auto b = best_bid();
    auto a = best_ask();
    if (b && a && *b >= *a)
        return fail("crossed book: bid " + std::to_string(*b) + " >= ask " + std::to_string(*a));

    return true;
}

void OrderBook::print_book(size_t levels) const {
    auto snap = depth(levels);
    snap.print();

    std::string err;
    if (!validate(&err)) {
        std::cout << "!! INVARIANT VIOLATION: " << err << "\n";
    }
}

OrderBook replay(std::string symbol, const std::vector<OrderCommand>& commands,
                 Price tick_size) {
    OrderBook book(std::move(symbol), tick_size);
    for (const auto& c : commands) {
        switch (c.kind) {
            case CommandKind::ADD_LIMIT:
                book.add_limit_order(c.side, c.price, c.qty, c.target_id);
                break;
            case CommandKind::ADD_MARKET:
                book.add_market_order(c.side, c.qty, c.target_id);
                break;
            case CommandKind::CANCEL:
                book.cancel_order(c.target_id);
                break;
            case CommandKind::MODIFY:
                book.modify_order(c.target_id, c.new_qty,
                                  c.has_new_price ? std::optional<Price>(c.new_price)
                                                  : std::nullopt);
                break;
        }
    }
    return book;
}
