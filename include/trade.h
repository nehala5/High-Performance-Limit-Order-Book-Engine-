#pragma once

#include "order.h"

struct Trade {
    uint64_t  trade_id;
    OrderId   aggressor_id;
    OrderId   passive_id;
    OrderSide side;
    Price     price;
    Quantity  qty;
    Timestamp timestamp;
};
