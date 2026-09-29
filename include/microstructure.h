#pragma once

#include "order.h"
#include "market_data.h"

#include <deque>
#include <optional>
#include <string>
#include <vector>

// One point-in-time view of the book, produced by the pipeline after every
// operation that actually changed something. This is the analyzer's only
// input: it deliberately knows nothing about the book, the feed or the
// pipeline, so the metric definitions can be read and tested in isolation.
//
// Timestamps must be microseconds and monotonically increasing. Mixing venue
// time with local wall-clock time in one series would silently corrupt every
// rate, so the pipeline prefers the feed's own clock whenever the line carries
// one, and falls back to a wall clock only when it does not.
struct Observation {
    SeqNum    seq = 0;
    uint64_t  ts_us = 0;

    // Top-N levels, best first: bids descending, asks ascending.
    std::vector<PriceLevel> bids, asks;

    bool     is_add = false, is_cancel = false, is_modify = false;
    bool     applied = false;
    Quantity order_qty = 0;    // size submitted by an add
    Quantity leaves_qty = 0;   // size withdrawn by a cancel, or left working

    uint32_t trade_count = 0;  // executions this operation caused
    Quantity trade_qty = 0;
    Quantity buy_qty = 0;      // buy-initiated: the aggressor was a buy
    Quantity sell_qty = 0;     // sell-initiated
    double   trade_notional = 0.0;
};

// Everything the analyzer knows at one point in the series. Formulas are
// documented at each field; they are not interchangeable with similar-sounding
// measures elsewhere, so the definitions are part of the contract.
struct Metrics {
    // --- Quote ---------------------------------------------------------
    std::optional<Price> best_bid, best_ask, mid_price;
    std::optional<double> spread;
    bool     has_mid = false;

    // --- Depth (top-N, unweighted) --------------------------------------
    // bid + ask quantity over the configured depth.
    Quantity bid_depth = 0, ask_depth = 0, total_depth = 0;
    // (bid - ask) / (bid + ask). Positive means more support below.
    // Undefined with an empty book, reported as 0.
    double   depth_imbalance = 0.0;

    // Depth weighted by nearness to the mid: level i counts 1/(1+|p-mid|).
    // Size far from the touch is not the same liquidity as size at it, so this
    // and depth_imbalance genuinely differ rather than being synonyms.
    // 0 on a one-sided book, since there is no mid to measure distance from.
    double   liquidity_imbalance = 0.0;

    // --- Trade ---------------------------------------------------------
    uint64_t trade_count = 0;
    Quantity trade_volume = 0, buy_volume = 0, sell_volume = 0;
    double   trade_notional = 0.0;
    // notional / volume over every execution so far.
    double   vwap = 0.0;
    // Executions per second, and shares per second, over elapsed time.
    double   trade_intensity = 0.0;
    double   volume_intensity = 0.0;
    double   avg_trade_size = 0.0;

    // --- Order flow ----------------------------------------------------
    // Cont, Kukanov & Stoikov (2014), summed over the series so far:
    //   step = dQbest_bid - dQbest_ask, where for a side
    //   dQ = (p_t >= p_prev) * q_t  -  (p_t <= p_prev) * q_prev
    // i.e. size added at an unchanged or better quote, minus size removed from
    // an unchanged or worse one. Net buying pressure at the touch.
    double   ofi = 0.0;
    // OFI divided by mean |step|, the scale-free form. 0 until two
    // observations exist.
    double   ofi_normalized = 0.0;

    // Easley, Lopez de Prado & O'Hara (2012): on a volume clock, the mean
    // absolute order imbalance per bucket divided by the bucket size.
    //   bucket imbalance = buy volume - sell volume
    //   VPIN = mean(|imbalance_i|) / bucket_volume
    // 0 until a whole bucket has closed.
    double   vpin = 0.0;
    uint64_t vpin_buckets = 0;

    // --- Volatility ----------------------------------------------------
    // Sum of squared log returns of the mid price, over the last `rv_window`
    // returns (all of them when the window is 0).
    double   realized_vol = 0.0;
    // RiskMetrics: v_t = lambda * v_{t-1} + (1 - lambda) * r_t^2.
    double   ewma_variance = 0.0;
    double   ewma_vol = 0.0;
    double   realized_vol_annualized = 0.0;
    double   ewma_vol_annualized = 0.0;
    uint64_t return_count = 0;

    // --- Arrivals and cancellations ------------------------------------
    uint64_t orders = 0, cancels = 0, modifies = 0;
    // cancels / (orders + cancels)
    double   cancel_rate = 0.0;
    // withdrawn size / submitted size
    double   cancel_rate_by_volume = 0.0;
    // New orders per second, and submitted shares per second.
    double   order_arrival_rate = 0.0;
    double   volume_arrival_rate = 0.0;

    uint64_t observations = 0;
    double   elapsed_seconds = 0.0;
};

struct MicrostructureConfig {
    // RiskModels' 0.94 is a daily figure. Intraday feeds want a smaller value
    // so recent moves are not drowned by old ones.
    double   ewma_lambda = 0.94;
    // Realized-vol window in returns; 0 keeps the whole series.
    size_t   rv_window = 0;
    // Levels counted for the depth and liquidity imbalance measures.
    size_t   imbalance_depth = 5;
    // VPIN bucket size, in shares. 0 derives it from the mean trade size so far.
    Quantity vpin_bucket_qty = 0;
    // Sampling periods per year, at this feed's rate. Scales the annualized
    // figures; meaningless unless it matches the actual cadence.
    double   periods_per_year = 252.0;
};

// Microstructure analytics, fed one Observation per book change.
//
// This is a streaming calculator. Order flow imbalance, VPIN, realized
// volatility and EWMA are all sequential -- each step depends on the previous
// quote, the previous volume, the previous return -- so they cannot be
// recomputed from a set of trades after the fact. Feed it in order.
class MicrostructureAnalyzer {
public:
    explicit MicrostructureAnalyzer(MicrostructureConfig cfg = {});

    void observe(const Observation& o);

    const Metrics& current() const { return m_; }
    const std::vector<Metrics>& history() const { return history_; }
    const MicrostructureConfig& config() const { return cfg_; }

    std::string report() const;

private:
    MicrostructureConfig cfg_;

    Metrics m_;
    std::vector<Metrics> history_;

    bool     started_ = false;
    uint64_t first_ts_us_ = 0, last_ts_us_ = 0;

    // OFI needs the previous touch, and whether each side was quoted at all.
    bool     have_prev_quote_ = false;
    bool     prev_has_bid_ = false, prev_has_ask_ = false;
    Price    prev_bid_ = 0.0, prev_ask_ = 0.0;
    Quantity prev_bid_qty_ = 0, prev_ask_qty_ = 0;

    // Volatility state.
    Price    prev_mid_ = 0.0;
    bool     have_prev_mid_ = false;
    std::deque<double> sq_returns_;
    double   rv_sum_ = 0.0;

    // VPIN's volume clock. The imbalance is signed -- buys minus sells -- so it
    // cannot be a Quantity: an unsigned subtraction would wrap.
    Quantity bucket_target_ = 0;
    Quantity bucket_volume_ = 0;     // total volume in the open bucket
    int64_t  bucket_imbalance_ = 0;  // buy - sell within it
    double   imbalance_abs_sum_ = 0.0;

    // Running mean trade size, used to size VPIN buckets when not configured.
    double   mean_trade_size_ = 0.0;

    double ofi_abs_sum_ = 0.0;
    uint64_t ofi_steps_ = 0;

    // Cumulative size submitted and withdrawn, for the volume-based
    // cancellation rate.
    Quantity submitted_qty_ = 0, withdrawn_qty_ = 0;

    void update_flow(const Observation& o);
    void update_trades(const Observation& o);
    void update_depth(const Observation& o);
    void update_volatility();
    void update_arrivals(const Observation& o, double elapsed);
};
