# High-Performance Limit Order Book Engine

A C++17 core model of a matching-engine order book with price-time priority. Built to be embedded as the trading core of a larger system.

## Features

- **Order Matching Engine** — price-time priority, FIFO within price level; stops when qty exhausted or no more cross
- **Order Types** — Limit (default) and Market (fills immediately at best available prices), each with its own matching logic
- **Partial Fills** — tracks `filled_qty` vs `qty`, updates status (`PENDING` / `PARTIAL` / `FILLED` / `CANCELLED`); remaining qty stays on the book
- **Trade Generation** — `Trade` struct (`aggressor_id`, `passive_id`, `price`, `qty`, `timestamp`), persisted trade journal, trades returned from the match operation
- **Cancellation** — remove by ID, fully off-book, status `CANCELLED`
- **Order Modification** — change qty; increasing qty re-queues and loses priority (cancel + re-add)
- **L2 Market Data** — snapshot of top N bid/ask levels with aggregated qty, mid price, spread, best bid/ask
- **Event Stream** — typed event log (`OrderAdded`, `OrderFilled`, `OrderCancelled`, `Trade`) for audit trail and replay capability

## Structure

```
include/order.h        Order model, enums, partial-fill helpers
include/trade.h        Trade struct
include/event.h        EventStream (std::variant event log)
include/market_data.h  L2Snapshot: PriceLevel, BBO, mid, spread
include/order_book.h   OrderBook public API
src/order_book.cpp     Matching engine and all operations
main.cpp               Interactive demo of every feature
test/test_book.cpp     Feature tests
```

## Build

```bash
cmake -S . -B build
cmake --build build
.\build\demo.exe          # feature demo
ctest --test-dir build    # run unit tests
```

## Matching semantics

Bids live in a levels map descending by price, asks ascending. Each price level holds a FIFO deque of order IDs, giving price-time priority (earliest order at the best price fills first). The matching loop walks the opposing book from the best level down, consuming until the incoming order's quantity is exhausted or the prices no longer cross.

## License

MIT