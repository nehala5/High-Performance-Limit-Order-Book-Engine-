#pragma once

#include "order_book.h"
#include "feed.h"
#include "microstructure.h"
#include "anomaly.h"

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

// The engine stamps orders and trades with a steady_clock: monotonic, cheap,
// and meaningless once the process exits. A record destined for Redis or
// Postgres needs a wall clock, or a replayed feed has no usable event time.
using WallClock = std::chrono::system_clock;

inline uint64_t epoch_micros_now() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            WallClock::now().time_since_epoch())
            .count());
}

// What the pipeline persisted. Deliberately flat and pointer-free, so these map
// straight onto a Redis hash or a Postgres row with no translation layer.
struct OrderEventRecord {
    SeqNum        seq           = 0;   // engine sequence this produced
    OrderId       order_id      = 0;
    FeedEventType type          = FeedEventType::NONE;
    bool          applied       = false;  // the engine accepted the operation
    OrderStatus   final_status  = OrderStatus::NEW;
    OrderSide     side          = OrderSide::BUY;
    OrderType     order_type    = OrderType::LIMIT;
    Price         price         = 0.0;
    Quantity      requested_qty = 0;
    Quantity      filled_qty    = 0;
    Quantity      leaves_qty    = 0;
    RejectReason  reject_code   = RejectReason::NONE;
    uint64_t      exchange_ts   = 0;     // venue clock, ns; 0 if the line had none
    uint64_t      recorded_at_us = 0;    // wall clock, when we stored it
    std::string   raw;                    // the source line, verbatim
};

struct TradeRecord {
    uint64_t  trade_id      = 0;
    SeqNum    seq           = 0;
    OrderId   aggressor_id  = 0;
    OrderId   passive_id    = 0;
    OrderSide side          = OrderSide::BUY;
    Price     price         = 0.0;
    Quantity  qty           = 0;
    uint64_t  exchange_ts   = 0;
    uint64_t  recorded_at_us = 0;
    bool      from_print    = false;   // a feed trade print, not engine-matched
};

struct SnapshotRecord {
    SeqNum                seq           = 0;
    uint64_t              exchange_ts   = 0;
    uint64_t              recorded_at_us = 0;
    std::optional<Price>  best_bid, best_ask, mid_price;
    std::optional<double> spread;
    std::vector<PriceLevel> bids, asks;
};

// Metrics derived from the recorded event stream rather than from the book, so
// the analytics layer is independent of the thing it measures.
struct Analytics {
    uint64_t lines_parsed    = 0;
    uint64_t lines_malformed = 0;
    uint64_t lines_ignored   = 0;   // blank, comment, unknown verb

    uint64_t adds = 0, cancels = 0, modifies = 0, trade_prints = 0;
    uint64_t adds_accepted = 0, adds_rejected = 0;
    uint64_t cancel_ok = 0, cancel_refused = 0;
    uint64_t modify_ok = 0, modify_refused = 0;

    uint64_t trades = 0;           // engine executions; prints excluded
    Quantity total_volume = 0;
    double   total_notional = 0.0;
    Quantity buy_volume = 0, sell_volume = 0;
    Price    high = 0.0, low = 0.0;
    bool     has_trades = false;

    Quantity submitted_qty = 0;    // quantity asked for
    Quantity filled_qty    = 0;    // quantity actually executed
    Quantity cancelled_qty = 0;    // quantity withdrawn before filling

    uint64_t snapshots = 0;
    double   mean_spread = 0.0, min_spread = 0.0, max_spread = 0.0;
    uint64_t snapshots_with_spread = 0;

    std::map<Price, Quantity> volume_by_price;
    Quantity largest_trade = 0;

    double vwap() const {
        return total_volume ? total_notional / static_cast<double>(total_volume) : 0.0;
    }
    double fill_rate() const {
        return submitted_qty ? static_cast<double>(filled_qty) /
                               static_cast<double>(submitted_qty)
                             : 0.0;
    }
    double cancel_rate() const {
        const uint64_t ops = adds + cancels + modifies;
        return ops ? static_cast<double>(cancels) / static_cast<double>(ops) : 0.0;
    }
    std::string to_string() const;
};

struct PipelineConfig {
    size_t    snapshot_depth   = 10;   // levels kept per snapshot
    uint64_t  snapshot_every   = 0;    // snapshot after every N applied ops; 0 = off
    bool      snapshot_on_add  = false;// snapshot after every accepted add
    size_t    observe_depth    = 5;    // levels handed to the microstructure analyzer
};

// Drives the book from a feed and records everything that happens.
//
//   market data -> FeedParser -> FeedEvent -> Pipeline -> OrderBook
//                                        -> OrderEventRecord / TradeRecord
//                                        -> Analytics
class Pipeline {
public:
    struct Outcome {
        bool        parsed  = false;   // the line was well formed
        bool        applied = false;   // the engine accepted the operation
        bool        ignored = false;   // blank, comment, or unknown verb
        std::string error;             // parse failure, or the engine's refusal
        FeedEventType type   = FeedEventType::NONE;
        // The engine's sequence after the call. A refused operation does not
        // consume a sequence, so this repeats the previous value on a refusal.
        SeqNum      seq      = 0;
        OrderId     order_id = 0;
        std::optional<Price> avg_price;
        Quantity    filled_qty = 0;
        Quantity    leaves_qty = 0;
        OrderStatus status   = OrderStatus::NEW;
        std::vector<Trade> trades;      // executions this line produced
    };

    explicit Pipeline(OrderBook& book, PipelineConfig cfg = {});

    // Parses and applies one line. A malformed line is reported and changes
    // nothing: not the book, and not the accepted-work counters.
    Outcome submit_line(std::string_view line);

    // Applies an already-parsed event, for callers holding a decoded feed.
    Outcome apply(const FeedEvent& ev);

    // Applies a whole blob, one Outcome per line that had content.
    std::vector<Outcome> submit_text(std::string_view text);

    // Attaches a microstructure analyzer. When set, every operation that
    // actually changed the book hands it an Observation, in order. A null
    // pointer (the default) costs nothing.
    void set_analyzer(MicrostructureAnalyzer* a) { analyzer_ = a; }
    MicrostructureAnalyzer* analyzer() const { return analyzer_; }

    // Attaches a spoofing detector. Like the analyzer, it only sees operations
    // the engine accepted: a refused order never existed, so there is nothing
    // to judge. An accepted order is judged when its fate is known -- cancelled
    // or filled -- not when it arrives, because arrival alone says nothing about
    // whether it was an attempt to move a price or to provide liquidity.
    void set_detector(SpoofDetector* d) { detector_ = d; }
    SpoofDetector* detector() const { return detector_; }

    void take_snapshot(uint64_t exchange_ts = 0);
    const SnapshotRecord& last_snapshot() const { return last_snapshot_; }

    const std::vector<OrderEventRecord>& order_events() const { return order_events_; }
    const std::vector<TradeRecord>&     trade_events() const { return trade_events_; }
    const std::vector<SnapshotRecord>&  snapshots()    const { return snapshots_; }

    Analytics analytics() const;

private:
    OrderBook&     book_;
    PipelineConfig cfg_;
    uint64_t       ops_since_snapshot_ = 0;

    uint64_t lines_parsed_    = 0;
    uint64_t lines_malformed_ = 0;
    uint64_t lines_ignored_   = 0;

    std::vector<OrderEventRecord> order_events_;
    std::vector<TradeRecord>     trade_events_;
    std::vector<SnapshotRecord>  snapshots_;
    SnapshotRecord               last_snapshot_;

    void maybe_snapshot(bool applied, FeedEventType type, uint64_t exchange_ts);

    // Emits one Observation to the analyzer, if one is attached. Called only
    // for operations that changed the book, which is what makes OFI a measure
    // of quote evolution rather than of feed noise.
    void emit_observation(const FeedEvent& ev, const Outcome& out,
                          const std::vector<Trade>& executed);

    // Emits one OrderActivity to the detector, if one is attached. Needs the
    // executions to attribute fills to the orders they lifted, and the book
    // after the operation as the context the order should be judged against.
    // `pre_mid` is captured before the operation is applied, because by the time
    // this runs the book already reflects the order's own effect.
    void emit_activity(const FeedEvent& ev, const Outcome& out,
                       const std::vector<Trade>& executed,
                       const std::optional<Price>& pre_mid);

    MicrostructureAnalyzer* analyzer_ = nullptr;
    SpoofDetector*           detector_ = nullptr;
};
