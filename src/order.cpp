#include "order.h"

const char* status_name(OrderStatus s) {
    switch (s) {
        case OrderStatus::NEW:             return "NEW";
        case OrderStatus::OPEN:            return "OPEN";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::FILLED:          return "FILLED";
        case OrderStatus::CANCELLED:       return "CANCELLED";
        case OrderStatus::REJECTED:        return "REJECTED";
    }
    return "UNKNOWN";
}
