# High-Performance Limit Order Book Engine

A C++17 core model of a matching-engine order book with price-time priority. Built to be embedded as the trading core of a larger system.

## Features

- **Order Matching Engine** — price-time priority, FIFO within price level; stops when qty exhausted or no more cross
- **Order Types** — Limit (default) and Market (fills immediately at best available prices), each with its own matching logic
- **Partial Fills** — tracks `filled_qty` vs `qty`, updates status (`PENDING` / `PARTIAL` / `FILLED` / `CANCELLED`); remaining qty stays on the book
- **Trade Generation** — `Trade` struct (`aggressor_id`, `passive_id`, `price`, `qty`, `timestamp`), persisted trade journal, trades returned from the match operation
- **Cancellation** — remove by ID, fully off-book, status `CANCELLED`
- **Order Modification** — quantity and/or price
- **Sequence Numbers** — every accepted request gets a monotonic `SeqNum`; orders and trades are tagged with the request that produced them
- **BBO Queries** — `best_bid` / `best_ask` / `mid_price` / `spread` as `std::optional`, so an empty book is distinguishable from a price of zero
- **Depth** — `depth(levels)` returns an L2 snapshot with per-level aggregated qty and order count; `queue(side, price)` returns the raw FIFO order ids at one level
- **Reconstruction** — an append-only command log plus `replay()` rebuilds a bit-identical book from the order stream
- **Invariant Checker** — `validate()` asserts the book is always internally consistent and never crossed
- **Event Stream** — typed event log (`OrderAdded`, `OrderFilled`, `OrderCancelled`, `OrderModified`, `Trade`) for audit trail

## Structure

```
include/order.h        Order model, enums, partial-fill helpers
include/trade.h        Trade struct
include/event.h        EventStream (std::variant audit log)
include/command.h      OrderCommand + CommandLog (the replayable input stream)
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

## Modification semantics

A modify is not a blind overwrite — the queue position is part of the order's identity:

| Change | Effect |
| --- | --- |
| Quantity decrease, same price | Queue position **kept**. Shrinking must not buy a better place in line. |
| Quantity increase | Re-queued at the back of the level, priority **lost**. |
| Price change | Order is removed from the old level and re-queued at the back of the new one, priority **lost**. Because a new price is a new order to the opposing side, it is matched immediately; whatever is left rests. |

A modify that would contradict quantity already filled (`new_qty <= filled_qty`), or that targets a filled, cancelled, unknown, or market order, is rejected and the book is untouched.

## Reconstruction

`OrderBook` keeps two logs with deliberately different jobs:

- `EventStream` — the **derived** audit trail. It contains events produced *by* matching (fills, trades). Replaying it would re-derive those and double-count.
- `CommandLog` — the **input** stream: exactly what each caller asked for, in arrival order. `replay(symbol, commands)` re-applies it to a fresh book.

Because matching is deterministic, replay reproduces the same order ids, the same queue order, and the same depth. This is asserted directly in the tests, including after a 4000-request randomised stream.

`validate()` is the other half of the guarantee. After every operation it checks that no level is empty, no order is overfilled or mislabelled, every resting order sits in exactly one level at the right price and side, no order is queued twice, and the book is never crossed. `print_book` runs it and reports any violation.

## License

MIT
