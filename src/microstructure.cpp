#include "microstructure.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace {

// Safe ratio: a zero denominator is reported as zero rather than inf/NaN, so a
// metric that is undefined for a given state stays comparable in a time series.
double ratio(double num, double den) {
    return den == 0.0 ? 0.0 : num / den;
}

}  // namespace

MicrostructureAnalyzer::MicrostructureAnalyzer(MicrostructureConfig cfg)
    : cfg_(cfg) {}

void MicrostructureAnalyzer::update_depth(const Observation& o) {
    const size_t n = cfg_.imbalance_depth;

    m_.bid_depth = 0;
    m_.ask_depth = 0;
    for (size_t i = 0; i < o.bids.size() && i < n; ++i) m_.bid_depth += o.bids[i].total_qty;
    for (size_t i = 0; i < o.asks.size() && i < n; ++i) m_.ask_depth += o.asks[i].total_qty;
    m_.total_depth = m_.bid_depth + m_.ask_depth;
    m_.depth_imbalance =
        ratio(static_cast<double>(m_.bid_depth) - static_cast<double>(m_.ask_depth),
              static_cast<double>(m_.total_depth));

    // Nearness weighting: a share at the touch is worth more than one ten ticks
    // away, so the two imbalance measures say different things.
    if (m_.has_mid && m_.total_depth > 0) {
        double wb = 0.0, wa = 0.0;
        for (size_t i = 0; i < o.bids.size() && i < n; ++i) {
            wb += o.bids[i].total_qty /
                  (1.0 + std::fabs(o.bids[i].price - *m_.mid_price));
        }
        for (size_t i = 0; i < o.asks.size() && i < n; ++i) {
            wa += o.asks[i].total_qty /
                  (1.0 + std::fabs(o.asks[i].price - *m_.mid_price));
        }
        m_.liquidity_imbalance = ratio(wb - wa, wb + wa);
    } else {
        m_.liquidity_imbalance = 0.0;
    }
}

// Cont, Kukanov & Stoikov's OFI, extended to one-sided books: a quote that
// appears from nothing is all addition, and one that vanishes is all removal.
void MicrostructureAnalyzer::update_flow(const Observation& o) {
    const bool has_bid = !o.bids.empty();
    const bool has_ask = !o.asks.empty();
    const Price  bid = has_bid ? o.bids[0].price : 0.0;
    const Price  ask = has_ask ? o.asks[0].price : 0.0;
    const Quantity bq = has_bid ? o.bids[0].total_qty : 0;
    const Quantity aq = has_ask ? o.asks[0].total_qty : 0;

    if (!have_prev_quote_) {
        prev_has_bid_ = has_bid; prev_bid_ = bid; prev_bid_qty_ = bq;
        prev_has_ask_ = has_ask; prev_ask_ = ask; prev_ask_qty_ = aq;
        have_prev_quote_ = true;
        return;
    }

    // new_size is the size sitting at a quote that improved or held; old_size
    // the size that was sitting at one that worsened or held. A quote that
    // appears is pure addition and one that is pulled is pure removal.
    auto side_delta = [](bool has_now, Price p_now, Quantity q_now,
                         bool has_prev, Price p_prev, Quantity q_prev) -> double {
        if (!has_now && !has_prev) return 0.0;
        if (!has_now) return -static_cast<double>(q_prev);
        if (!has_prev) return static_cast<double>(q_now);
        const double new_size = (p_now >= p_prev) ? static_cast<double>(q_now) : 0.0;
        const double old_size = (p_now <= p_prev) ? static_cast<double>(q_prev) : 0.0;
        return new_size - old_size;
    };

    const double delta_bid =
        side_delta(has_bid, bid, bq, prev_has_bid_, prev_bid_, prev_bid_qty_);
    const double delta_ask =
        side_delta(has_ask, ask, aq, prev_has_ask_, prev_ask_, prev_ask_qty_);

    const double step = delta_bid - delta_ask;
    m_.ofi += step;
    ofi_abs_sum_ += std::fabs(step);
    ++ofi_steps_;
    m_.ofi_normalized = ratio(m_.ofi, ofi_abs_sum_ / static_cast<double>(ofi_steps_));

    prev_has_bid_ = has_bid; prev_bid_ = bid; prev_bid_qty_ = bq;
    prev_has_ask_ = has_ask; prev_ask_ = ask; prev_ask_qty_ = aq;
}

void MicrostructureAnalyzer::update_trades(const Observation& o) {
    m_.trade_count += o.trade_count;
    m_.trade_volume += o.trade_qty;
    m_.buy_volume  += o.buy_qty;
    m_.sell_volume += o.sell_qty;
    m_.trade_notional += o.trade_notional;

    m_.vwap = ratio(m_.trade_notional, static_cast<double>(m_.trade_volume));
    m_.avg_trade_size =
        ratio(static_cast<double>(m_.trade_volume), static_cast<double>(m_.trade_count));
}

// Reads the mid price off the metrics the caller has already set, so the
// sequence only advances on observations that actually had a two-sided book.
void MicrostructureAnalyzer::update_volatility() {
    if (!m_.has_mid) return;

    if (!have_prev_mid_) {
        prev_mid_ = *m_.mid_price;
        have_prev_mid_ = true;
        return;
    }

    if (*m_.mid_price <= 0.0 || prev_mid_ <= 0.0) {
        prev_mid_ = *m_.mid_price;
        return;
    }

    const double r = std::log(*m_.mid_price / prev_mid_);
    const double r2 = r * r;
    prev_mid_ = *m_.mid_price;
    ++m_.return_count;

    // Realized variance over a rolling window of squared returns.
    sq_returns_.push_back(r2);
    rv_sum_ += r2;
    if (cfg_.rv_window && sq_returns_.size() > cfg_.rv_window) {
        rv_sum_ -= sq_returns_.front();
        sq_returns_.pop_front();
    }
    m_.realized_vol = rv_sum_;

    // EWMA: seeded from the first return, so it is defined from the second
    // observation onward rather than dragging up from zero.
    const double lambda = cfg_.ewma_lambda;
    m_.ewma_variance = (m_.return_count == 1)
                           ? r2
                           : lambda * m_.ewma_variance + (1.0 - lambda) * r2;
    m_.ewma_vol = std::sqrt(std::max(0.0, m_.ewma_variance));

    m_.realized_vol_annualized =
        std::sqrt(std::max(0.0, m_.realized_vol) * cfg_.periods_per_year);
    m_.ewma_vol_annualized = m_.ewma_vol * std::sqrt(cfg_.periods_per_year);
}

void MicrostructureAnalyzer::update_arrivals(const Observation& o, double elapsed) {
    if (o.is_add && o.applied) {
        ++m_.orders;
        submitted_qty_ += o.order_qty;
        // The bucket size is the mean execution size so far, which is the usual
        // bulk-volume estimate.
        if (m_.trade_count > 0) {
            mean_trade_size_ = static_cast<double>(m_.trade_volume) /
                               static_cast<double>(m_.trade_count);
        }
    } else if (o.is_cancel && o.applied) {
        ++m_.cancels;
        withdrawn_qty_ += o.leaves_qty;
    } else if (o.is_modify && o.applied) {
        ++m_.modifies;
    }

    const uint64_t flow_ops = m_.orders + m_.cancels;
    m_.cancel_rate = ratio(static_cast<double>(m_.cancels), static_cast<double>(flow_ops));
    m_.cancel_rate_by_volume =
        ratio(static_cast<double>(withdrawn_qty_), static_cast<double>(submitted_qty_));

    m_.order_arrival_rate = ratio(static_cast<double>(m_.orders), elapsed);
    m_.volume_arrival_rate = ratio(static_cast<double>(submitted_qty_), elapsed);
    m_.trade_intensity = ratio(static_cast<double>(m_.trade_count), elapsed);
    m_.volume_intensity = ratio(static_cast<double>(m_.trade_volume), elapsed);
}

void MicrostructureAnalyzer::observe(const Observation& o) {
    m_.observations += 1;

    if (!started_) {
        started_ = true;
        first_ts_us_ = o.ts_us;
    }
    last_ts_us_ = o.ts_us;
    m_.elapsed_seconds =
        last_ts_us_ >= first_ts_us_
            ? static_cast<double>(last_ts_us_ - first_ts_us_) / 1e6
            : 0.0;

    m_.best_bid = o.bids.empty() ? std::nullopt
                                 : std::optional<Price>(o.bids[0].price);
    m_.best_ask = o.asks.empty() ? std::nullopt
                                 : std::optional<Price>(o.asks[0].price);
    if (m_.best_bid && m_.best_ask) {
        m_.mid_price = (*m_.best_bid + *m_.best_ask) / 2.0;
        m_.spread = *m_.best_ask - *m_.best_bid;
        m_.has_mid = true;
    } else {
        m_.mid_price.reset();
        m_.spread.reset();
        m_.has_mid = false;
    }

    update_depth(o);
    update_trades(o);
    update_flow(o);
    update_volatility();

    // --- VPIN, on a volume clock -----------------------------------------
    if (o.trade_qty > 0) {
        // The bucket size is fixed for the whole series, which is the point of a
        // volume clock. Deriving it afresh from a running mean would let the
        // boundaries drift -- and a shrinking target drives the bucket volume
        // past the target, so every share would close a bucket and the measure
        // would degenerate. V is set once, from the first trade, then held.
        if (bucket_target_ == 0) {
            const double first_size = static_cast<double>(o.trade_qty) /
                                      static_cast<double>(std::max(1u, o.trade_count));
            bucket_target_ = cfg_.vpin_bucket_qty
                                 ? cfg_.vpin_bucket_qty
                                 : static_cast<Quantity>(std::max(1.0, first_size));
        }

        Quantity remaining = o.trade_qty;
        while (remaining > 0) {
            const Quantity room =
                (bucket_volume_ < bucket_target_) ? (bucket_target_ - bucket_volume_) : 0;
            if (room == 0) {           // defensive: never spin on a full bucket
                imbalance_abs_sum_ += std::fabs(static_cast<double>(bucket_imbalance_));
                ++m_.vpin_buckets;
                bucket_volume_ = 0;
                bucket_imbalance_ = 0;
                continue;
            }
            const Quantity take = std::min(remaining, room);
            // Split the observation's buy/sell in proportion to the fill.
            const double frac = static_cast<double>(take) /
                                static_cast<double>(o.trade_qty);
            bucket_imbalance_ += static_cast<int64_t>(
                                    static_cast<double>(o.buy_qty) * frac) -
                                static_cast<int64_t>(
                                    static_cast<double>(o.sell_qty) * frac);
            bucket_volume_ += take;
            remaining -= take;

            if (bucket_volume_ >= bucket_target_) {
                imbalance_abs_sum_ += std::fabs(static_cast<double>(bucket_imbalance_));
                ++m_.vpin_buckets;
                bucket_volume_ = 0;
                bucket_imbalance_ = 0;
            }
        }
    }
    // Undefined until a whole bucket has closed, and 0 rather than NaN.
    m_.vpin = m_.vpin_buckets
                  ? ratio(imbalance_abs_sum_ / static_cast<double>(m_.vpin_buckets),
                          static_cast<double>(bucket_target_))
                  : 0.0;

    update_arrivals(o, m_.elapsed_seconds);

    history_.push_back(m_);
}

std::string MicrostructureAnalyzer::report() const {
    std::ostringstream os;
    os << std::fixed << std::setprecision(4);

    os << "--- quote ---\n";
    os << "  best bid / ask : " << (m_.best_bid ? std::to_string(*m_.best_bid) : std::string("-"))
       << " / " << (m_.best_ask ? std::to_string(*m_.best_ask) : std::string("-"))
       << "\n";
    os << "  mid            : " << (m_.has_mid ? std::to_string(*m_.mid_price) : std::string("-"))
       << "   spread: " << (m_.spread ? std::to_string(*m_.spread) : std::string("-"))
       << "\n";

    os << "--- depth (top " << cfg_.imbalance_depth << ") ---\n";
    os << "  bid / ask / total : " << m_.bid_depth << " / " << m_.ask_depth << " / "
       << m_.total_depth << "\n";
    os << "  depth imbalance    : " << m_.depth_imbalance
       << "  (1 = all bid, -1 = all ask)\n";
    os << "  liquidity imbal.   : " << m_.liquidity_imbalance
       << "  (size weighted by nearness to mid)\n";

    os << "--- trade ---\n";
    os << "  executions     : " << m_.trade_count << "\n";
    os << "  volume         : " << m_.trade_volume << "   buy " << m_.buy_volume
       << " / sell " << m_.sell_volume << "\n";
    os << "  vwap           : " << m_.vwap << "\n";
    os << "  intensity      : " << m_.trade_intensity << " trades/s, "
       << m_.volume_intensity << " shares/s\n";
    os << "  avg trade size : " << m_.avg_trade_size << "\n";

    os << "--- order flow ---\n";
    os << "  OFI            : " << m_.ofi << "   normalized: " << m_.ofi_normalized
       << "  (1 = pure buy pressure)\n";
    os << "  VPIN           : " << m_.vpin << "  over " << m_.vpin_buckets
       << " bucket(s) of " << bucket_target_ << "\n";

    os << "--- volatility ---\n";
    os << "  returns        : " << m_.return_count << "\n";
    os << "  realized vol   : " << m_.realized_vol << "   annualized: "
       << m_.realized_vol_annualized << "\n";
    os << "  EWMA vol       : " << m_.ewma_vol << "   annualized: "
       << m_.ewma_vol_annualized << "   lambda " << cfg_.ewma_lambda << "\n";

    os << "--- arrivals ---\n";
    os << "  orders / cancels / modifies : " << m_.orders << " / " << m_.cancels
       << " / " << m_.modifies << "\n";
    os << "  arrival rate   : " << m_.order_arrival_rate << " orders/s, "
       << m_.volume_arrival_rate << " shares/s\n";
    os << "  cancel rate    : " << m_.cancel_rate << "  (by orders), "
       << m_.cancel_rate_by_volume << "  (by volume)\n";
    os << "  window         : " << m_.elapsed_seconds << "s over " << m_.observations
       << " observations\n";

    return os.str();
}
