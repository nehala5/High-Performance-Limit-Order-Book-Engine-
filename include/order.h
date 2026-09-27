#pragma once

#include <cstdint>
#include <chrono>
#include <string>

enum class OrderSide : uint8_t { BUY = 0, SELL = 1 };

enum class OrderType : uint8_t { LIMIT = 0, MARKET = 1 };

enum class OrderStatus : uint8_t {
    PENDING   = 0,
    PARTIAL   = 1,
    FILLED    = 2,
    CANCELLED = 3
};

using OrderId   = uint64_t;
using Price     = double;
using Quantity  = uint64_t;
using SeqNum    = uint64_t;
using Timestamp = std::chrono::steady_clock::time_point;

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

    // Still live: may be cancelled, modified or matched again.
    bool is_active() const {
        return status == OrderStatus::PENDING || status == OrderStatus::PARTIAL;
    }

    // Occupies a slot in a price level queue. Market orders never rest, so an
    // unsatisfied market order is active but not resting.
    bool is_resting() const {
        return is_active() && type == OrderType::LIMIT && remaining() > 0;
    }

    std::string side_str() const { return side == OrderSide::BUY ? "BUY" : "SELL"; }
    std::string type_str() const { return type == OrderType::LIMIT ? "LIMIT" : "MARKET"; }
    std::string status_str() const {
        switch (status) {
            case OrderStatus::PENDING:   return "PENDING";
            case OrderStatus::PARTIAL:   return "PARTIAL";
            case OrderStatus::FILLED:    return "FILLED";
            case OrderStatus::CANCELLED: return "CANCELLED";
        }
        return "UNKNOWN";
    }
};
