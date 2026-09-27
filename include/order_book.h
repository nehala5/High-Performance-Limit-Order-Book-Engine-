#pragma once

#include "order.h"
#include "trade.h"
#include "event.h"
#include "market_data.h"
#include "command.h"

#include <map>
#include <unordered_map>
#include <deque>
#include <vector>
#include <functional>
#include <optional>
#include <string>

class OrderBook {
public:
    explicit OrderBook(std::string symbol);

    const std::string& symbol() const;

    // --- Operations -------------------------------------------------------
    OrderId add_limit_order(OrderSide side, Price price, Quantity qty);
    OrderId add_market_order(OrderSide side, Quantity qty);

    bool cancel_order(OrderId id);

    // Shrinking an order keeps its queue position. Growing it, or changing the
    // price, re-queues it at the back of the (new) level and loses priority.
    bool modify_order(OrderId id, Quantity new_qty,
                      std::optional<Price> new_price = std::nullopt);

    const Order* get_order(OrderId id) const;

    // --- BBO / state queries ---------------------------------------------
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    std::optional<Price> mid_price() const;
    std::optional<Price> spread() const;
    L2Snapshot           depth(size_t levels = 10) const;

    // GET DEPTH. Alias kept for the existing call sites.
    L2Snapshot snapshot(size_t depth = 10) const { return this->depth(depth); }

    // Resting order ids at one price in queue order, front first. This is the
    // L3 view of a single level, and it is exactly what price-time priority
    // is defined over.
    std::vector<OrderId> queue(OrderSide side, Price price) const;

    // Monotonic counter, bumped once per accepted mutating request. The
    // sequence number a caller observes is what orders and trades are tagged
    // with, and is what a replay is checked against.
    SeqNum sequence() const;

    const EventStream& event_stream() const;
    const CommandLog&  command_log() const;

    size_t bid_levels() const;
    size_t ask_levels() const;
    size_t total_orders() const;

    // Structural invariants. Returns false and writes the first violation to
    // `err` if the book is not reconstructable from its own order set.
    bool validate(std::string* err = nullptr) const;

    void print_book(size_t depth = 5) const;

private:
    // A queue of resting order ids at one price. Distinct from the aggregated
    // market_data::PriceLevel, which is a reporting row rather than book state.
    struct Level {
        std::deque<OrderId> orders;
    };

    std::string symbol_;
    OrderId next_order_id_;
    uint64_t next_trade_id_;
    SeqNum  next_seq_;

    std::unordered_map<OrderId, Order> orders_;

    std::map<Price, Level, std::greater<Price>> bids_;
    std::map<Price, Level, std::less<Price>>    asks_;

    EventStream events_;
    std::vector<Trade> trades_;
    CommandLog  commands_;

    bool price_crosses(OrderSide side, Price incoming, Price resting) const;
    std::vector<Trade> match_order(Order& incoming);

    void add_to_book(Order& order);
    void remove_from_book(OrderId id);
    void requeue_in_book(OrderId id);
    void update_order_status(Order& order, Quantity fill_qty);

    // Aggregated quantity resting at `price` on `side`.
    Quantity level_qty(OrderSide side, Price price) const;
};

// Rebuilds a book by re-applying a command log to a fresh instance. Matching
// is deterministic, so the result is identical to the book that produced the
// log -- same order ids, same queue order, same depth.
OrderBook replay(std::string symbol, const std::vector<OrderCommand>& commands);
