#pragma once

#include "order.h"

#include <string>
#include <string_view>
#include <vector>

// The four things that can happen to an order.
enum class FeedEventType : uint8_t {
    NONE        = 0,   // blank or comment line: parsed fine, does nothing
    ADD         = 1,
    CANCEL      = 2,
    MODIFY      = 3,
    TRADE_PRINT = 4,
    UNKNOWN     = 5,   // unrecognised verb: parsed fine, refused
};

inline const char* feed_type_name(FeedEventType t) {
    switch (t) {
        case FeedEventType::NONE:        return "NONE";
        case FeedEventType::ADD:         return "ADD";
        case FeedEventType::CANCEL:      return "CANCEL";
        case FeedEventType::MODIFY:      return "MODIFY";
        case FeedEventType::TRADE_PRINT: return "TRADE";
        case FeedEventType::UNKNOWN:     return "UNKNOWN";
    }
    return "UNKNOWN";
}

// One instruction lifted off the wire. The engine never sees this type; the
// pipeline translates it into book operations.
struct FeedEvent {
    FeedEventType type = FeedEventType::NONE;

    OrderSide side;                        // ADD
    OrderType order_type = OrderType::LIMIT;
    Price     price      = 0.0;           // ADD (LIMIT only)
    Quantity  qty        = 0;              // ADD
    OrderId   order_id   = 0;              // ADD, CANCEL, MODIFY

    Quantity  new_qty       = 0;           // MODIFY
    Price     new_price     = 0.0;         // MODIFY
    bool      has_new_price = false;

    // Trade prints name both sides of the execution.
    OrderId   buy_order_id  = 0;            // TRADE
    OrderId   sell_order_id = 0;            // TRADE

    uint64_t  exchange_ts = 0;             // venue clock, nanoseconds; 0 if absent

    std::string verb;                      // as it appeared, for audit
    std::string raw;                       // the source line
};

struct FeedParseResult {
    bool        ok    = false;   // false means malformed: the book is untouched
    std::string error;
    FeedEvent   event;
    size_t      line_no = 0;
};

// Line-oriented feed parser.
//
//   ADD    <BUY|SELL> LIMIT  <price> <qty> <order_id>
//   ADD    <BUY|SELL> MARKET <qty> <order_id>
//   CANCEL <order_id>
//   MODIFY <order_id> <new_qty> [new_price]
//   TRADE  <price> <qty> <buy_order_id> <sell_order_id>
//
// Blank lines are ignored, and everything from a '#' to end of line is a
// comment -- so an inline note can be left on a line without changing it. Any
// line may carry a `ts=<nanoseconds>` venue timestamp anywhere in its fields.
//
// The parser checks syntax and types only. A zero quantity or a NaN price is
// well formed and reaches the engine, which refuses it through the ordinary
// validation gate -- so every refusal is accounted for as engine behaviour
// rather than disappearing at the parsing stage.
class FeedParser {
public:
    static FeedParseResult parse(std::string_view line, size_t line_no = 0);

    // Splits on newlines and parses each. Lines with no content are dropped, so
    // line numbers stay meaningful to the caller.
    static std::vector<FeedParseResult> parse_all(std::string_view text);
};
