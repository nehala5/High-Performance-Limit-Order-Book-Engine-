#pragma once

#include "order.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <optional>

struct PriceLevel {
    Price    price;
    Quantity total_qty;
    size_t   order_count;
};

struct L2Snapshot {
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    std::optional<Price>    best_bid;
    std::optional<Price>    best_ask;
    std::optional<Price>    mid_price;
    std::optional<double>   spread;

    void print() const {
        std::cout << "--- Asks (reversed) ---\n";
        for (int i = static_cast<int>(asks.size()) - 1; i >= 0; --i) {
            const auto& l = asks[i];
            std::cout << "  " << std::fixed << std::setprecision(2) << l.price
                      << "  qty=" << l.total_qty
                      << "  n=" << l.order_count << "\n";
        }
        std::cout << "--- Bids ---\n";
        for (const auto& l : bids) {
            std::cout << "  " << std::fixed << std::setprecision(2) << l.price
                      << "  qty=" << l.total_qty
                      << "  n=" << l.order_count << "\n";
        }
        if (best_bid) std::cout << "Best Bid: " << *best_bid << "\n";
        if (best_ask) std::cout << "Best Ask: " << *best_ask << "\n";
        if (mid_price) std::cout << "Mid:      " << *mid_price << "\n";
        if (spread)    std::cout << "Spread:   " << *spread << "\n";
    }
};
