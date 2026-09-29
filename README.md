# High-Performance Limit Order Book Engine

A C++17 core model of a matching-engine order book with price-time priority. Built to be embedded as the trading core of a larger system.

## Features

- **Order Matching Engine** — price-time priority, FIFO within price level; stops when qty exhausted or no more cross
- **Order Types** — Limit (default) and Market (fills immediately at best available prices), each with its own matching logic
- **Explicit Lifecycle** — `NEW → OPEN → PARTIALLY_FILLED → FILLED`, or `NEW → OPEN → CANCELLED`; every transition is checked against a transition table and terminal states are final
- **Order Validation** — every submission passes a gate before it touches the book: positive quantity, finite positive price, tick-grid conformance, unique id
- **Unique Order IDs** — auto-assigned, or caller-supplied and rejected if already taken, which makes retries idempotent
- **Execution Reports** — every submission returns what it traded: per-fill detail (`resting_id`, `price`, `qty`), volume-weighted `avg_price`, `filled_qty`, `leaves_qty`, and final status
- **Order History** — the ordered list of state transitions an order actually went through
- **Trade History** — the full journal, plus per-order queries, cumulative volume and notional
- **Partial Fills** — tracks `filled_qty` vs `qty`; remaining qty stays on the book
- **Cancellation** — cancel by ID; refused for filled, cancelled, unknown or rejected orders
- **Order Modification** — quantity and/or price, with the new price held to the same validation as a new order's
- **Sequence Numbers** — every accepted request gets a monotonic `SeqNum`; orders, trades and lifecycle steps are all tagged with the request that produced them
- **BBO Queries** — `best_bid` / `best_ask` / `mid_price` / `spread` as `std::optional`, so an empty book is distinguishable from a price of zero
- **Depth** — `depth(levels)` returns an L2 snapshot with per-level aggregated qty and order count; `queue(side, price)` returns the raw FIFO order ids at one level
- **Reconstruction** — an append-only command log plus `replay()` rebuilds a bit-identical book from the order stream
- **Event / Market Data Pipeline** — a text feed of `ADD` / `CANCEL` / `MODIFY` / `TRADE` lines, parsed into order events, applied to the book, and recorded as order events, trade events and book snapshots
- **Analytics** — volume, VWAP, price range, buy/sell split, fill and cancel rates, per-price volume and spread statistics, derived from the recorded event stream
- **Microstructure Analytics** — spread, mid, depth, depth and liquidity imbalance, OFI, VPIN, realized and EWMA volatility, order arrival rate, cancellation rate, trade intensity
- **Anomaly / Spoofing Detection** — large-order, rapid-cancellation, repetition, layering and price-manipulation rules over the order stream, with an explainable anomaly score
- **Invariant Checker** — `validate()` asserts the book is always internally consistent and never crossed
- **Event Stream** — typed event log (`OrderAdded`, `OrderFilled`, `OrderCancelled`, `OrderModified`, `OrderRejected`, `Trade`) for audit trail

## Structure

```
include/order.h        Order model, lifecycle states, transition table
include/trade.h        Trade struct
include/execution.h    Fill, RejectReason, ExecutionReport (the engine's output)
include/event.h        EventStream (std::variant audit log)
include/command.h      OrderCommand + CommandLog (the replayable input stream)
include/market_data.h  L2Snapshot: PriceLevel, BBO, mid, spread
include/order_book.h   OrderBook public API
include/feed.h         FeedEvent, FeedParser, the wire format
include/pipeline.h     Records, Analytics, Pipeline
include/microstructure.h  Observation, Metrics, MicrostructureAnalyzer
include/anomaly.h         OrderActivity, OrderProfile, AnomalySignal, Alert
src/order_book.cpp     Validation, lifecycle and all operations
src/feed.cpp           Line-oriented feed parser
src/pipeline.cpp       Feed -> book -> records -> analytics
src/microstructure.cpp Microstructure metrics, computed streaming
src/anomaly.cpp      Rule-based spoofing detection
main.cpp               Interactive demo of every feature
test/test_book.cpp         Engine tests
test/test_pipeline.cpp     Parser, pipeline and analytics tests
test/test_microstructure.cpp  Microstructure metric tests
test/test_anomaly.cpp      Spoofing detection tests
```

## Build

```bash
cmake -S . -B build
cmake --build build
.\build\demo.exe          # feature demo
ctest --test-dir build    # run unit tests
```

## Order lifecycle

```
NEW ──> OPEN ──> PARTIALLY_FILLED ──> FILLED
 │        └────────────────────────> CANCELLED
 │        └────────────────────────> PARTIALLY_FILLED ──> CANCELLED
 └─> REJECTED
```

`NEW` means accepted but not yet working; `OPEN` means resting on the book. An order can be matched the instant it is accepted, so it may skip `OPEN` entirely. `FILLED`, `CANCELLED` and `REJECTED` are terminal — nothing reopens a finished order.

Transitions are not free-form. `can_transition_to(from, to)` in `order.h` is the single authority, `set_status` is the only path that changes a status, and it records the step. `validate()` replays each order's recorded history to confirm the chain is legal and ends at the status the order currently reports, so an illegal transition cannot hide.

`order_history(id)` returns the steps that actually happened:

```
seq=1  NEW -> OPEN
seq=2  OPEN -> PARTIALLY_FILLED
seq=3  PARTIALLY_FILLED -> FILLED
```

## Order validation

Nothing reaches the book without passing the gate. A refused order is reported, not thrown, and the attempt is still recorded in the order map, the event stream and the command log — so the id resolves, the reason is auditable, and replay reproduces the refusal.

| Check | `RejectReason` | Why it matters |
| --- | --- | --- |
| `qty > 0` | `QUANTITY_NOT_POSITIVE` | A zero-quantity order is simultaneously "fully filled" and unfilled, which contradicts itself. |
| Price is finite | `PRICE_NOT_FINITE` | `NaN` as a `std::map<double>` key breaks the comparator's strict weak ordering. Levels silently merge and vanish from depth output — undefined behaviour, not a wrong answer. |
| Price is positive | `PRICE_NOT_POSITIVE` | Zero and negative prices are not tradeable, and would let a "bid at zero" masquerade as a real price. |
| Price is on the tick grid | `OFF_TICK` | Optional per-book `tick_size`; prices off the grid are refused. |
| Id is unused | `DUPLICATE_ORDER_ID` | A retried submission must not overwrite the original order. |

The same gate runs on a modify's new price, so an existing order cannot be repriced somewhere invalid.

## Order ids

`add_limit_order` assigns the next id when you pass none. Pass an explicit id and it is used verbatim, and rejected if already taken — which makes retries idempotent:

```cpp
auto a = book.add_limit_order(OrderSide::BUY, 100, 10, 500);
assert(a.order_id == 500);

auto retry = book.add_limit_order(OrderSide::BUY, 101, 10, 500);
assert(retry.rejected());
assert(retry.reject_code == RejectReason::DUPLICATE_ORDER_ID);
assert(book.get_order(500)->price == 100.0);   // untouched
```

Auto-assigned ids are pushed above any id the caller has already used, so the two can never collide. A refused duplicate does not become a second order — the id was taken.

## Matching semantics

Bids live in a levels map descending by price, asks ascending. Each price level holds a FIFO deque of order IDs, giving price-time priority (earliest order at the best price fills first). The matching loop walks the opposing book from the best level down, consuming until the incoming order's quantity is exhausted or the prices no longer cross.

Worked example:

```
ASK 105 -> 100 shares
ASK 106 -> 200 shares

BUY 150 @ 105
```

The bid can only lift the 105 offer, so it takes all 100 there and the remaining 50 rests as a new bid at 105. The 106 level is never touched — a limit order may not trade through its own limit:

```
ASK 106 -> 200        (unchanged)
BID 105 ->  50        (the resting remainder)
```

Had the order been a market order, or a limit at 106 or better, it would have continued into the next level.

Rules the engine holds to:

- **Price improvement is impossible.** Every fill prints at the *resting* order's price, never the aggressor's limit.
- **Best price first, then oldest first.** Levels are consumed best-to-worst; within a level, queue order decides.
- **A partial fill never costs an order its place.** A partly filled order keeps its slot at its price level.
- **What does not fill becomes the book.** Only the unfilled remainder of a limit order rests; a market order's remainder is discarded.
- **Matching never trades against a dead order.** The loop skips any queue entry that is not genuinely resting, so the engine cannot fill a cancelled or filled order even if the queues were to disagree.
- **The book is never left crossed.** Repositioning an order to a new price re-matches it first, and the invariant checker enforces it after every operation.

## Execution reports

`add_limit_order` and `add_market_order` return an `ExecutionReport` rather than a bare order id, because an id alone says nothing about what traded:

```cpp
auto r = book.add_limit_order(OrderSide::BUY, 106, 150);
// r.status      == FILLED
// r.filled_qty  == 150,  r.leaves_qty == 0
// r.match_count() == 2
// r.fills[0]    == {resting_id 1, price 105, qty 100}
// r.fills[1]    == {resting_id 2, price 106, qty 50}
// *r.avg_price() == 105.33
```

A refused submission is reported rather than thrown or silently ignored: `r.accepted()` is false, `r.status` is `REJECTED`, and `r.reject_reason` says why. The attempt is still recorded in both the order map and the audit trail, so the id resolves and replay reproduces the rejection.

The same trades are readable in bulk via `trades()`, or scoped to one request via `trades_since(seq)`, which works because each trade carries the sequence number of the request that produced it.


## Modification semantics

A modify is not a blind overwrite — the queue position is part of the order's identity:

| Change | Effect |
| --- | --- |
| Quantity decrease, same price | Queue position **kept**. Shrinking must not buy a better place in line. |
| Quantity increase | Re-queued at the back of the level, priority **lost**. |
| Price change | Order is removed from the old level and re-queued at the back of the new one, priority **lost**. Because a new price is a new order to the opposing side, it is matched immediately; whatever is left rests. |

A modify that would contradict quantity already filled (`new_qty <= filled_qty`), or that targets a filled, cancelled, unknown, or market order, is rejected and the book is untouched.

## Trade history

Every execution is journaled with the sequence number of the request that caused it, so the output of one submission can be isolated from the rest:

```cpp
SeqNum before = book.sequence();
auto r = book.add_limit_order(OrderSide::BUY, 106, 150);

book.trades_since(before);     // just this submission's executions
book.trades_for_order(r.order_id);   // everything one order was involved in
book.total_volume();           // cumulative shares traded
book.total_notional();         // cumulative price x qty
```

`trades_for_order` covers both sides: an order filled as the passive side, then repriced so it crosses, appears in its own history as passive first and aggressor second.

## Market data pipeline

So far the book has been driven by hand. The pipeline drives it from a feed
instead, and records what happens on the way through.

```
market data -> FeedParser -> FeedEvent -> Pipeline -> OrderBook
                                         -> OrderEventRecord
                                         -> TradeRecord
                                         -> SnapshotRecord
                                         -> Analytics
```

### Feed format

Line oriented, one instruction per line:

```
ADD    <BUY|SELL> LIMIT  <price> <qty> <order_id>
ADD    <BUY|SELL> MARKET <qty> <order_id>
CANCEL <order_id>
MODIFY <order_id> <new_qty> [new_price]
TRADE  <price> <qty> <buy_order_id> <sell_order_id>
```

A market order is not asked for a price, because it has none to send. Blank
lines are skipped, and everything from a `#` to end of line is a comment, so a
line can carry its own annotation. Any line may carry a venue timestamp as
`ts=<nanoseconds>` in any position — it is pulled out before positional parsing,
so it can never be mistaken for a field.

```cpp
OrderBook book("AAPL", 0.01);
Pipeline pipe(book);

auto outcomes = pipe.submit_text(R"(
    # AAPL
    ADD SELL LIMIT 105.00 100 1 ts=1700000000000000000
    ADD BUY  LIMIT 105.00 120 2 ts=1700000000300000000
    ADD BUY  MARKET  25 3         ts=1700000000800000000
)");

for (const auto& o : outcomes) { /* parsed / applied / filled / leaves / seq */ }
```

### Parser and engine draw the line at syntax

The parser checks syntax and types. It does not judge whether a submission makes
sense — a zero quantity or a `NaN` price is a well formed line that reaches the
engine, which refuses it through the ordinary validation gate. That split means
every refusal is visible as engine behaviour, and `RejectReason` is the single
place a reason is spelled.

A line the parser *cannot* read is a different thing entirely: it is reported
with its line number and changes nothing at all — not the book, not the stored
events, not the accepted-work counters. A bad feed is a feed problem, and it
never half-applies.

```cpp
auto o = pipe.submit_line("ADD SELL LIMIT oops 10 8");
o.parsed;    // false -- "price 'oops' is not a number"
o.applied;   // false -- nothing was touched
```

An unrecognised verb is neither malformed nor applied: it parses, and is
counted as ignored.

### What gets stored

Three append-only vectors, all flat and pointer-free so they map straight onto
a database row or a Redis hash:

| Record | One per | Carries |
| --- | --- | --- |
| `OrderEventRecord` | every ADD, CANCEL, MODIFY and print | `seq`, order id, final status, side, price, requested/filled/leaves qty, `RejectReason`, venue `ts`, wall-clock `recorded_at_us`, the raw line |
| `TradeRecord` | every execution | `trade_id`, `seq`, both order ids, aggressor side, price, qty, venue `ts`, wall clock |
| `SnapshotRecord` | on demand, or every N applied ops | `seq`, BBO, mid, spread, truncated depth |

`seq` is the engine's own sequence number, so a stored event points at exactly
the state change that produced it. `exchange_ts` is the venue's clock; that is
the only real event time, since the engine stamps orders with a `steady_clock`,
which is monotonic and meaningless once the process exits. `recorded_at_us` is
a wall clock, and exists so a persisted row has usable time of its own.

`raw` keeps the source line verbatim. Combined with the engine's own
`command_log()`, that is enough to reconstruct a session and explain it.

### TRADE is a print, not a fill

A `TRADE` line is a consolidated tape print: something the feed says happened
elsewhere. It is recorded, with `from_print = true`, and deliberately **not**
fed to the book. The engine's own executions are the authority; a print is
something to reconcile them against, and letting prints into the book would
double-count every fill. Analytics exclude them:

```cpp
Analytics a = pipe.analytics();
a.trades;         // 5   engine executions
a.trade_prints;   // 1   recorded, excluded from the above
```

### Analytics

Computed from the recorded event stream, not from the book, so the layer
measuring the engine is independent of the engine:

```cpp
a.total_volume;        // shares executed
a.vwap();               // volume-weighted average price
a.high, a.low;          // traded range
a.buy_volume, a.sell_volume;      // aggressor split
a.volume_by_price[104.0];         // size traded at each level
a.fill_rate();         // filled / submitted quantity
a.cancel_rate();       // cancels as a share of all operations
a.mean_spread;         // across snapshots that had a two-sided book
a.largest_trade;
```

`cancel_ok` versus `cancel_refused` is taken from the engine's return value, not
from the order's status. A cancel aimed at a FILLED order fails while the
order's status stays FILLED, so reading the status would score the failure as a
success.

### Feed path and replay path must agree

The pipeline writes to the same command log as any other caller, so a book
driven entirely from text is reconstructable:

```cpp
OrderBook rebuilt = replay("AAPL", book.command_log().all(), 0.01);
assert(book.depth(50).bids.size() == rebuilt.depth(50).bids.size());
assert(book.sequence() == rebuilt.sequence());
```

This is asserted in the test suite over a feed that includes malformed lines,
duplicate ids, off-tick prices, a cancel, a modify, and a market sweep. If the
feed path ever diverged from the replay path, reconstruction would be a lie.

### Where Redis and Postgres fit

The records are the seam. They are flat, they are ordered, and each one already
carries the sequence number and both timestamps a store would key or index on.

- **Redis** — a stream (`XADD`) per symbol, or a hash per order id for the
  current state with the event stream alongside it. Order events and trade
  events are append-only, so they are a natural fit; `seq` is the natural
  consumer offset.
- **Postgres** — partitioned `order_events` and `trade_events` by time, with
  `trades_for_order` covered by `(order_id, seq)`. Snapshots are wide and few,
  so they want compression or columnar storage rather than the same treatment.
- **Both** — the usual shape is Redis for the hot recent tail and Postgres for
  durable history, with the sequence number as the handoff point.

Writes want to be batched, so the natural next step is a drain interface over
these three vectors rather than a per-event call. That is deliberately not
built here: it is a real design decision about durability and back-pressure, and
a stub that pretends to be one would be worse than its absence.

## Market microstructure analytics

The pipeline records what happened. This computes what it *means* — quote
state, book pressure, order flow, and volatility — from the same feed.

```cpp
OrderBook book("AAPL", 0.01);
Pipeline pipe(book);
MicrostructureAnalyzer ana;
pipe.set_analyzer(&ana);

pipe.submit_text(feed);
std::cout << ana.report();
```

`set_analyzer` hands the analyzer one `Observation` per operation that actually
changed the book. Refused and malformed lines produce nothing, because nothing
changed — an observation per feed line would measure the feed, not the market.

### Basic metrics

| Metric | Definition |
| --- | --- |
| `best_bid` / `best_ask` | touch, or empty |
| `mid_price` | `(bid + ask) / 2`, undefined on a one-sided book |
| `spread` | `ask - bid`, undefined on a one-sided book |
| `bid_depth` / `ask_depth` | summed size over the top N levels |
| `total_depth` | both sides |
| `depth_imbalance` | `(bid - ask) / (bid + ask)`, +1 = all bid |
| `liquidity_imbalance` | as above, but each level weighted `1/(1+\|p - mid\|)` |
| `trade_volume`, `buy_volume`, `sell_volume` | executed shares, split by aggressor |
| `vwap` | `notional / volume` over every execution |
| `trade_intensity` | executions per second, and shares per second |

`depth_imbalance` and `liquidity_imbalance` are not synonyms. "Liquidity
imbalance" is used loosely in the literature; here it is explicitly the
nearness-weighted form, because ten shares ten ticks away are not the same
liquidity as ten shares at the touch. A book with equal total size but lopsided
placement scores 0 on one and strongly signed on the other.

### Advanced metrics

**Order Flow Imbalance** follows Cont, Kukanov & Stoikov (2014). For each side:

```
dQ = (p_t >= p_prev) * q_t  -  (p_t <= p_prev) * q_prev
step = dQ_best_bid - dQ_best_ask
```

Size added at an unchanged or better quote, minus size removed from an unchanged
or worse one. Summed over the series, and also normalized by the mean absolute
step. It measures *order flow*, not trade direction — withdrawing the ask
removes supply and reads as positive, which is the opposite of what "sell
pressure" would suggest. A quote appearing from nothing counts as all addition
and one that vanishes as all removal, so a one-sided book is not read as
inactivity.

**VPIN** follows Easley, López de Prado & O'Hara (2012), on a volume clock:

```
bucket imbalance = buy volume - sell volume
VPIN = mean(|imbalance_i|) / bucket_volume
```

Buckets are fixed volume, not fixed time, which is what makes VPIN robust to a
quiet or busy period. The bucket size is set once from the first trade and then
held: re-deriving it from a running mean lets the boundaries drift, and a
shrinking target drives the accumulated volume past the target, so every share
closes a bucket and the measure collapses. VPIN is 0 until a whole bucket has
closed — a partial bucket is not a measurement.

**Realized volatility** sums squared log returns of the mid price over a rolling
window. **EWMA volatility** is the RiskModels recursion
`v_t = λ·v_{t-1} + (1-λ)·r_t²`, seeded from the first return. RiskMetrics' 0.94
is a *daily* constant; intraday feeds want a smaller λ, and the config exposes
it. Both are also reported annualized by `√(·periods_per_year)` — which is only
meaningful if you set `periods_per_year` to your actual sampling cadence.

**Order arrival rate** (orders/second and shares/second), **cancellation rate**
(by order count and by volume), and **trade intensity** (executions/second and
shares/second) follow from the same clock. A refused operation counts for
none of them.

### The clock is a real footgun

`Observation::ts_us` is microseconds and must be monotonic. The pipeline prefers
the feed's own `ts=` and falls back to a wall clock only when a line carries
none. Every rate in the analyzer is a count divided by elapsed time, so a feed
timestamped in the wrong unit does not fail loudly — it produces confident,
wrong rates. If you connect a real feed, confirm the unit once.

`history()` returns a full `Metrics` per observation, which is the actual
product: one row per book change, ready to chart or store. A long-running system
would ring-buffer or downsample that rather than keep it all.

## Anomaly and spoofing detection

Rule-based, over the order events the pipeline already collects. No ML, by
design: the rules are legible, tunable, and you can argue with them.

```cpp
OrderBook book("AAPL", 0.01);
Pipeline pipe(book);
SpoofDetector det;
pipe.set_detector(&det);

pipe.submit_text(feed);
std::cout << det.report();      // alerts, each with the rules that fired
```

The pipeline hands the detector one `OrderActivity` per accepted operation,
carrying the order, the book before and after, and the executions it caused.
Refused operations never arrive: an order the engine rejected never existed,
and there is nothing to judge.

An order is scored when its fate is known — cancelled or filled — not when it
arrives. Arrival alone says nothing about intent, and evaluating too early
would flag every large resting order in the book.

### The hard part is not detection

A market maker posting a large order and cancelling it 100 ms later is the
mechanism they use to make a living, several thousand times a day. A detector
that flags that is worse than no detector, because it will be switched off. Four
things follow, and they are the actual design:

**"Large" is statistical, not absolute.** 900 lots is unremarkable on one
instrument and enormous on another, so size is a z-score against recent
placements on the same book, with a `large_min_qty` floor so a quiet book
cannot flag 1 lots. Two details matter more than the threshold:

- The baseline is **winsorised**, so it learns the bulk of the distribution
  rather than its tail. Without this, a spoofer repeating the same trick slowly
  trains the detector to consider the trick normal — the order that has done it
  six times is the one that finally looks ordinary.
- It is **slow** (`alpha = 0.005`, an effective window of ~200 orders) and runs
  a plain running mean for the first ten. An EW mean at `alpha = 0.05` has a
  window of ~20 orders, so a handful of block trades redefine "typical" within
  the same round they are being compared against.

**"Favourable" is signed by side.** A large sell wants the price down; a large
buy wants it up. `mid_move_favor` is signed so that positive always means the
mid moved the way *this placer* wanted. An unsigned "how far did the price move"
would flag every seller in the market.

**Context modulates; only evidence counts.** Rules split into two kinds.
*Evidence* is about this order: `large_order`, `rapid_cancellation`,
`repeated_placement`, `price_manipulation`, `layering`. *Context* is about the
window it happened in: the cancellation-to-placement ratio, and this order's
share of recent flow. Context multiplies the score but never counts toward the
evidence threshold — otherwise a benign order posted in a busy book inherits
suspicion from its neighbours, and the cap that exists to prevent false
positives would itself manufacture them.

**No single signal can raise an alert.** Evidence combines by noisy-OR, then
the total is capped by how many *distinct* rules fired:

```
score = 100 * noisy_or(evidence) * min(1, n_evidence / min_signals) * context_gain
```

`min_signals = 2` means one signal alone can never exceed 50. On top of that,
`require_pattern` insists on repetition or price movement before an alert is
raised at all. A large, fast, unfilled order 30 ticks from the mid trips three
rules and stays silent; six of them in a pattern does not.

### The rules

| Rule | Fires when |
| --- | --- |
| `large_order` | size is a z-score outlier against recent placements, above the floor |
| `rapid_cancellation` | a large order withdrawn inside the threshold, never filled |
| `repeated_placement` | N+ large fast unfilled orders, same side, same price band, inside the cluster window |
| `price_manipulation` | the mid moved in the placer's favour by at least `impact_min_ticks` while the order was up |
| `layering` | a large order withdrawn unfilled that sat at least `layering_min_ticks` from the mid |
| `cancel_ratio` | *(context)* cancellations per placement, as excess over the reference |
| `flow_concentration` | *(context)* this order's share of the window's absolute flow |

Clusters key on side and a price *band* rather than an exact price: the same
trick repeated at 105.00 and 105.01 is one pattern, and two clusters running
opposite ways at the same price are two patterns, not one of double the size.

### What it will not tell you

- **Attribution.** Rules score an *order id*, not a participant. Connecting that
  to an account needs an identifier the feed does not currently carry.
- **Intent.** Every signal here is circumstantial. A score is a reason to look,
  not a conclusion, which is why each alert decomposes into the numbers that
  produced it.
- **Regimes.** Thresholds are per-instrument config. They are not calibrated
  against any real venue's data, so the defaults are structurally sensible and
  empirically arbitrary. Tune them against a sample of normal trading before
  trusting the output.
- **A market order.** It never rests, so it has no lifetime and no cancellation
  to make. Spoofing is a resting-order behaviour; aggressive flow is the
  microstructure analyzer's job.

## Reconstruction

`OrderBook` keeps two logs with deliberately different jobs:

- `EventStream` — the **derived** audit trail. It contains events produced *by* matching (fills, trades). Replaying it would re-derive those and double-count.
- `CommandLog` — the **input** stream: exactly what each caller asked for, in arrival order. `replay(symbol, commands)` re-applies it to a fresh book.

Because matching is deterministic, replay reproduces the same order ids, the same queue order, and the same depth. This is asserted directly in the tests, including after a 4000-request randomised stream.

`validate()` is the other half of the guarantee. After every operation it checks that no level is empty, no order is overfilled or mislabelled, every resting order sits in exactly one level at the right price and side, no order is queued twice, and the book is never crossed. `print_book` runs it and reports any violation.

## License

MIT
