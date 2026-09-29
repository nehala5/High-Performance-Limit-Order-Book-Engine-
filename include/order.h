#pragma once

#include <cstdint>
#include <chrono>
#include <cmath>
#include <string>

enum class OrderSide : uint8_t { BUY = 0, SELL = 1 };

enum class OrderType : uint8_t { LIMIT = 0, MARKET = 1 };

//   NEW -> OPEN -> PARTIALLY_FILLED -> FILLED
//              \-> CANCELLED
//   NEW -> REJECTED
//
// NEW is accepted but not yet working; OPEN is resting on the book. A terminal
// state is final -- nothing leaves FILLED, CANCELLED or REJECTED.
enum class OrderStatus : uint8_t {
    NEW              = 0,
    OPEN             = 1,
    PARTIALLY_FILLED = 2,
    FILLED           = 3,
    CANCELLED        = 4,
    REJECTED         = 5
};

using OrderId   = uint64_t;
using Price     = double;
using Quantity  = uint64_t;
using SeqNum    = uint64_t;
using Timestamp = std::chrono::steady_clock::time_point;

constexpr bool is_terminal(OrderStatus s) {
    return s == OrderStatus::FILLED || s == OrderStatus::CANCELLED ||
           s == OrderStatus::REJECTED;
}

constexpr bool can_transition_to(OrderStatus from, OrderStatus to) {
    if (is_terminal(from)) return false;   // terminal is final
    if (from == to) return true;           // restating a working state is fine
    switch (from) {
        case OrderStatus::NEW:
            // An order can be matched the instant it is accepted, so it may
            // skip OPEN entirely.
            return to == OrderStatus::OPEN || to == OrderStatus::PARTIALLY_FILLED ||
                   to == OrderStatus::FILLED  || to == OrderStatus::CANCELLED ||
                   to == OrderStatus::REJECTED;
        case OrderStatus::OPEN:
        case OrderStatus::PARTIALLY_FILLED:
            return to == OrderStatus::PARTIALLY_FILLED || to == OrderStatus::FILLED ||
                   to == OrderStatus::CANCELLED;
        default:
            return false;
    }
}

const char* status_name(OrderStatus s);

// One recorded step in an order's life.
struct OrderTransition {
    OrderId     order_id;
    SeqNum      seq;
    OrderStatus from;
    OrderStatus to;
    Timestamp   timestamp;
};

struct Order {
    OrderId     id;
    SeqNum      seq;
    OrderSide   side;
    OrderType   type;
    OrderStatus status;
    Price       price;
    Quantity    qty;
    Quantity    filled_qty;
    Timestamp   timestamp;

    Quantity remaining() const { return qty - filled_qty; }
    bool is_fully_filled() const { return filled_qty >= qty; }

    // Accepted by the book, as opposed to refused at the door.
    bool is_accepted() const { return status != OrderStatus::REJECTED; }

    // Working: eligible to be matched, and the only cancellable/modifiable
    // states besides NEW.
    bool is_working() const {
        return status == OrderStatus::OPEN || status == OrderStatus::PARTIALLY_FILLED;
    }

    bool is_terminal() const {
        return status == OrderStatus::FILLED || status == OrderStatus::CANCELLED ||
               status == OrderStatus::REJECTED;
    }

    // Occupies a slot in a price level queue. Market orders never rest, so an
    // unsatisfied market order is working but not resting.
    bool is_resting() const {
        return is_working() && type == OrderType::LIMIT && remaining() > 0;
    }

    bool can_transition_to(OrderStatus to) const { return ::can_transition_to(status, to); }

    // A price the book can safely key a level on. Market orders carry a
    // placeholder, so only limit prices are held to the grid.
    bool price_is_sane() const {
        if (!std::isfinite(price)) return false;
        return type != OrderType::LIMIT || price > 0.0;
    }

    std::string side_str() const { return side == OrderSide::BUY ? "BUY" : "SELL"; }
    std::string type_str() const { return type == OrderType::LIMIT ? "LIMIT" : "MARKET"; }
    std::string status_str() const { return status_name(status); }
};
