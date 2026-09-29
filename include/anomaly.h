#pragma once

#include "order.h"
#include "market_data.h"

#include <deque>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// One engine operation, as the detector needs to see it: what was attempted, the
// book immediately after it, and any executions it caused.
struct OrderActivity {
    enum class Action : uint8_t { PLACE = 0, CANCEL = 1, MODIFY = 2 };

    SeqNum    seq = 0;
    uint64_t  ts_us = 0;

    Action    action = Action::PLACE;
    bool      applied = false;
    OrderId   order_id = 0;
    OrderSide side = OrderSide::BUY;
    OrderType type = OrderType::LIMIT;
    Price     price = 0.0;
    Quantity  qty = 0;         // submitted on PLACE, withdrawn on CANCEL
    OrderStatus status = OrderStatus::NEW;

    // Book context immediately after the operation.
    std::vector<PriceLevel> bids, asks;

    // The mid *before* the operation. Separate from the fields above because an
    // aggressive order moves the book as it arrives, and reading its placement
    // context from the post-operation book measures it against a market it had
    // already pushed. For a resting order the two agree.
    std::optional<Price> prev_mid;

    // Executions this operation caused, from either side. Both order ids are
    // carried: the passive one because it was lifted, the aggressor one because
    // it may have been the order this very operation placed.
    struct Fill {
        OrderId   passive_id = 0;
        OrderId   aggressor_id = 0;
        Quantity  qty = 0;
        Price     price = 0.0;
        OrderSide aggressor = OrderSide::BUY;
    };
    std::vector<Fill> fills;
};

// A single rule firing, with the number that produced it. Kept so an alert can
// be argued with: a score nobody can decompose is a score nobody can trust.
struct AnomalySignal {
    std::string name;
    double      strength = 0.0;   // 0..1
    double      weight   = 0.0;   // its contribution to the score
    bool        context  = false; // true if it describes the flow, not the order
    std::string detail;

    bool fired() const { return strength > 0.0; }
};

// The full state of one order, from placement to whatever ended it.
struct OrderProfile {
    OrderId     id = 0;
    OrderSide   side = OrderSide::BUY;
    OrderType   type = OrderType::LIMIT;
    Price       price = 0.0;
    Quantity    qty = 0;
    Quantity    filled_qty = 0;

    uint64_t placed_ts_us = 0;
    SeqNum   placed_seq = 0;
    uint64_t ended_ts_us = 0;
    OrderStatus end_status = OrderStatus::NEW;

    // Context at placement, which is the only fair baseline for judging it.
    std::optional<Price> mid_at_place;
    double   spread_at_place = 0.0;
    Quantity depth_at_place = 0;
    // Ticks between the order and the mid. Large values are the layering tell.
    double   ticks_from_mid = 0.0;
    // True if it was priced through the opposite touch, so leaving it unfilled
    // was a choice rather than an accident.
    bool     crossed_touch = false;

    double   size_zscore = 0.0;
    bool     large = false;

    // Filled in when the order ends.
    double   lifetime_us = 0.0;
    std::optional<Price> mid_at_end;
    // Signed movement in the placer's favour while the order was up. A
    // spoofing sell wants the price to fall; that shows up here as positive.
    double   mid_move_favor = 0.0;
    bool     ended = false;

    bool completed_unfilled() const { return ended && filled_qty == 0; }
};

struct AnomalyConfig {
    // --- Size ------------------------------------------------------------
    // A "large" order is a statistical outlier against recent placements, not
    // an absolute number: 500 shares is unremarkable on one instrument and
    // enormous on another. The floor stops a quiet book from flagging 1 lots.
    // The warmup stops the first handful of orders being called outliers
    // against a baseline that has barely formed.
    double   large_zscore = 3.0;
    Quantity large_min_qty = 10;
    // Deliberately slow. An EW mean at alpha = 0.05 has an effective window of
    // roughly twenty orders, so a few block trades redefine "typical" within
    // the same round they are being compared against -- the demo's own mean
    // walked 26 -> 107 over six rounds for exactly that reason. A size baseline
    // has to remember more than the last few orders to mean anything.
    double   baseline_alpha = 0.005;
    // The first `warmup` orders use a plain running mean (alpha = 1/n) so the
    // baseline actually converges instead of still climbing when the first real
    // order arrives; the slow EW takes over after that.
    uint64_t size_baseline_warmup = 10;
    // The baseline tracks the bulk of the distribution, not its tail. A
    // spoofer who repeats the same trick would otherwise teach the detector
    // that the trick is normal, and grow steadily less suspicious doing it.
    // Winsorising the input at this multiple of the running mean keeps the tail
    // from moving the mean, while a sustained regime change still gets learned
    // -- slowly, which is the correct pace for "this is now normal".
    double   baseline_winsor = 4.0;

    // --- Lifetime --------------------------------------------------------
    uint64_t rapid_cancel_us = 2000000;   // 2s

    // --- Repetition ------------------------------------------------------
    uint64_t cluster_window_us = 60000000;   // 60s
    size_t   repeat_min = 3;                 // occurrences to count as a pattern
    Price    cluster_band_ticks = 1.0;       // same-side, within this many ticks
    size_t   repeat_max = 6;                 // occurrences that max out the signal

    // --- Manipulation ----------------------------------------------------
    double   impact_min_ticks = 1.0;   // mid must move this far to count
    double   impact_max_ticks = 5.0;   // this many ticks maxes the signal
    double   layering_min_ticks = 2.0; // placed this far from the mid

    // --- Flow ------------------------------------------------------------
    // What share of the window's absolute order flow this one order carried.
    // Deliberately not a z-score of order size: that would just restate
    // `large_order`. Concentration is a different question -- not "is this
    // order big" but "is this one order the reason the flow looks like this".
    double   flow_share_min = 0.35;
    // Cancellation-to-placement ratio, scored as *excess over a reference*.
    // Deliberately a reference rather than a threshold on the raw ratio: the
    // raw ratio depends heavily on how much of the book simply rests and stays,
    // so an absolute cut-off means something different on every instrument.
    double   cancel_ratio_baseline = 0.90;
    double   cancel_ratio_span = 0.08;

    // --- Window and scoring ----------------------------------------------
    uint64_t window_us = 300000000;    // 5m, for the ratio features
    Price    tick_size = 0.01;
    size_t   imbalance_depth = 5;
    double   min_signals = 2.0;        // one signal alone cannot reach `alert_score`
    bool     require_pattern = true;   // ...and repetition or price movement is
                                       //    required before an alert is raised
    double   alert_score = 60.0;       // 0..100
    size_t   max_tracked = 100000;     // bound on working orders held
};

struct Alert {
    SeqNum   seq = 0;
    uint64_t ts_us = 0;
    double   score = 0.0;            // 0..100
    OrderId  order_id = 0;
    bool     pattern = false;        // repetition or price movement present
    std::string summary;
    std::vector<AnomalySignal> signals;
    std::vector<OrderId> related_orders;   // the cluster this order belongs to
};

// Rule-based spoofing and anomaly detection over the order event stream.
//
// The central problem is not detection, it is false positives. A market maker
// posting large orders and cancelling them within milliseconds is the mechanism
// they use to make a living, and a detector that flags that is worse than none.
// Three things follow from it:
//
//   1. "Large" is statistical, judged against recent placements on the same
//      book, with an absolute floor so a quiet book cannot flag 1 lots.
//   2. Lifetime is judged against the order's role. A large order that trades
//      is doing its job; the same order sitting unfilled while the price moved
//      in the placer's favour is doing something else.
//   3. No single signal can raise an alert. The score is capped by how many
//      *distinct* rules fired, so the common case -- one big fast cancel --
//      stays quiet, and a pattern of them does not.
class SpoofDetector {
public:
    explicit SpoofDetector(AnomalyConfig cfg = {});

    // Feeds one operation. Call in engine order.
    void observe(const OrderActivity& a);

    const AnomalyConfig& config() const { return cfg_; }
    const std::vector<Alert>& alerts() const { return alerts_; }
    const std::vector<AnomalySignal>& last_signals() const { return last_signals_; }
    const OrderProfile* profile(OrderId id) const;

    // Series-level counters, useful for tuning and for the demo.
    uint64_t orders_seen() const { return orders_seen_; }
    uint64_t orders_large() const { return orders_large_; }
    uint64_t rapid_cancels() const { return rapid_cancels_; }
    uint64_t alerts_raised() const { return alerts_.size(); }
    uint64_t orders_closed() const { return closed_.size(); }

    // Cancellations per placement across the window. Note this is the *raw
    // stream* ratio, so a book full of orders that simply rest and stay will
    // pull it well below 1. The often-quoted "market makers cancel 95% of what
    // they post" is a ratio over the market maker's own quote-reposting, and is
    // not the same quantity; `cancel_ratio_baseline` below is the reference
    // point for this one.
    double cancel_to_placement_ratio() const;
    double mean_order_lifetime_us() const;
    double mean_large_lifetime_us() const;

    // Rates measured over the configured window, for reporting.
    double placements_per_second() const;
    double cancellations_per_second() const;

    std::string report() const;

private:
    AnomalyConfig cfg_;

    // Rolling baseline of placed order size. A cold baseline yields no z-score
    // at all rather than a confident one, via size_n_ vs the warmup setting.
    double   size_mean_ = 0.0;
    double   size_var_ = 0.0;
    uint64_t size_n_ = 0;

    // Working orders, with an intrusive FIFO so the map stays bounded without
    // ever going quadratic to find or remove the oldest entry. A plain deque of
    // ids cannot be pruned on completion, so it would grow forever while the map
    // it guards stayed small.
    using Fifo = std::list<std::pair<uint64_t, OrderId>>;
    std::unordered_map<OrderId, OrderProfile> working_;
    std::unordered_map<OrderId, Fifo::iterator> working_pos_;
    Fifo working_fifo_;

    // Placements, cancellations and signed flow inside the window.
    struct Stamp { uint64_t ts_us; Quantity qty; };
    std::deque<Stamp>  window_placements_;
    std::deque<Stamp>  window_cancels_;
    std::deque<std::pair<uint64_t, double>> window_flow_;

    // Short-lived large orders on the same side near the same price, for
    // repetition clustering.
    struct ClusterKey {
        OrderSide side;
        int64_t  price_bucket;
        bool operator==(const ClusterKey& o) const {
            return side == o.side && price_bucket == o.price_bucket;
        }
    };
    struct ClusterKeyLess {
        bool operator()(const ClusterKey& a, const ClusterKey& b) const {
            return a.side != b.side ? static_cast<int>(a.side) < static_cast<int>(b.side)
                                     : a.price_bucket < b.price_bucket;
        }
    };
    std::map<ClusterKey, std::deque<std::pair<uint64_t, OrderId>>, ClusterKeyLess> clusters_;

    std::vector<AnomalySignal> last_signals_;
    std::vector<Alert>         alerts_;
    std::deque<OrderProfile>   closed_;   // capped history, for lifetime stats

    uint64_t orders_seen_ = 0;
    uint64_t orders_large_ = 0;
    uint64_t rapid_cancels_ = 0;

    double   lifetime_sum_us_ = 0.0;
    double   large_lifetime_sum_us_ = 0.0;
    uint64_t lifetime_n_ = 0;

    void update_baselines(const OrderActivity& a);
    void evict(uint64_t now_us);
    void track_placement(const OrderActivity& a);
    // The order id is passed separately rather than read off the activity: for a
    // cancel the two agree, but an execution names the *aggressor* in
    // `order_id` while the profile that just ended is the one that was lifted.
    void end_order(OrderId id, const OrderActivity& a, OrderStatus status);
    void apply_fills(const OrderActivity& a);
    void maybe_alert(const OrderProfile& p, uint64_t ts_us,
                      const std::vector<AnomalySignal>& signals,
                      const std::vector<OrderId>& cluster);

    std::vector<AnomalySignal> evaluate(const OrderProfile& p, uint64_t now_us) const;
    ClusterKey cluster_key_for(const OrderProfile& p) const;
};
