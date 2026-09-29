#pragma once

#include "order.h"

#include <optional>
#include <string>
#include <vector>

// One execution against a single resting order.
struct Fill {
    OrderId  resting_id;
    Price    price;
    Quantity qty;
};

// Why the book refused a submission. Typed so a caller can branch on it
// instead of matching on message text.
enum class RejectReason : uint8_t {
    NONE                 = 0,
    QUANTITY_NOT_POSITIVE = 1,
    PRICE_NOT_POSITIVE   = 2,
    PRICE_NOT_FINITE     = 3,
    OFF_TICK             = 4,
    DUPLICATE_ORDER_ID   = 5,
};

inline const char* reason_name(RejectReason r) {
    switch (r) {
        case RejectReason::NONE:                  return "";
        case RejectReason::QUANTITY_NOT_POSITIVE: return "quantity must be positive";
        case RejectReason::PRICE_NOT_POSITIVE:   return "price must be positive";
        case RejectReason::PRICE_NOT_FINITE:     return "price must be finite";
        case RejectReason::OFF_TICK:             return "price is not a multiple of the tick size";
        case RejectReason::DUPLICATE_ORDER_ID:   return "order id is already in use";
    }
    return "unknown";
}

// What the matching engine did with a single submission. This is the engine's
// output: an order id on its own says nothing about what traded or at what
// price, so every mutating entry point hands one of these back.
struct ExecutionReport {
    OrderId      order_id      = 0;
    SeqNum       seq           = 0;
    OrderStatus  status        = OrderStatus::REJECTED;
    OrderType    type          = OrderType::LIMIT;
    Quantity     requested_qty = 0;
    Quantity     filled_qty    = 0;
    Quantity     leaves_qty    = 0;   // still working
    RejectReason reject_code   = RejectReason::NONE;
    std::vector<Fill> fills;

    bool accepted()    const { return status != OrderStatus::REJECTED; }
    bool fully_filled() const { return accepted() && leaves_qty == 0; }
    size_t match_count() const { return fills.size(); }

    bool rejected() const { return !accepted(); }
    std::string reject_reason() const { return reason_name(reject_code); }
    std::string status_str() const { return status_name(status); }

    // Volume-weighted average execution price, or nothing if nothing traded.
    std::optional<Price> avg_price() const {
        if (fills.empty()) return std::nullopt;
        double notional = 0.0;
        double qty = 0.0;
        for (const auto& f : fills) {
            notional += f.price * static_cast<double>(f.qty);
            qty      += static_cast<double>(f.qty);
        }
        if (qty == 0.0) return std::nullopt;
        return notional / qty;
    }
};
