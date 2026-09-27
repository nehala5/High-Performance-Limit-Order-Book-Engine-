#pragma once

#include "order.h"

#include <iostream>
#include <vector>

enum class CommandKind : uint8_t {
    ADD_LIMIT  = 0,
    ADD_MARKET = 1,
    CANCEL     = 2,
    MODIFY     = 3,
};

// The *input* side of the book: exactly what a caller asked for, recorded in
// arrival order. This is deliberately distinct from EventStream (the derived
// audit trail) because replaying derived events would re-derive them and
// double-count fills. Replaying this log reproduces the book exactly.
struct OrderCommand {
    CommandKind kind          = CommandKind::ADD_LIMIT;
    OrderSide   side          = OrderSide::BUY;
    Price       price         = 0.0;   // ADD_LIMIT
    Quantity    qty           = 0;     // ADD_LIMIT, ADD_MARKET
    OrderId     target_id     = 0;     // CANCEL, MODIFY; assigned id for ADD
    Quantity    new_qty       = 0;     // MODIFY
    Price       new_price     = 0.0;   // MODIFY
    bool        has_new_price = false; // MODIFY
    SeqNum      seq           = 0;
    Timestamp   timestamp{};
};

struct CommandLog {
    std::vector<OrderCommand> commands;

    void record(const OrderCommand& c) { commands.push_back(c); }

    const std::vector<OrderCommand>& all() const { return commands; }
    size_t size() const { return commands.size(); }
    void clear() { commands.clear(); }

    void print() const {
        for (size_t i = 0; i < commands.size(); ++i) {
            const auto& c = commands[i];
            std::cout << "[" << i << "] seq=" << c.seq << "  ";
            switch (c.kind) {
                case CommandKind::ADD_LIMIT:
                    std::cout << "ADD LIMIT  " << (c.side == OrderSide::BUY ? "BUY " : "SELL")
                              << " px=" << c.price << " qty=" << c.qty
                              << " -> id=" << c.target_id;
                    break;
                case CommandKind::ADD_MARKET:
                    std::cout << "ADD MARKET " << (c.side == OrderSide::BUY ? "BUY " : "SELL")
                              << " qty=" << c.qty << " -> id=" << c.target_id;
                    break;
                case CommandKind::CANCEL:
                    std::cout << "CANCEL     id=" << c.target_id;
                    break;
                case CommandKind::MODIFY:
                    std::cout << "MODIFY     id=" << c.target_id
                              << " qty=" << c.new_qty;
                    if (c.has_new_price) std::cout << " px=" << c.new_price;
                    break;
            }
            std::cout << "\n";
        }
    }
};
