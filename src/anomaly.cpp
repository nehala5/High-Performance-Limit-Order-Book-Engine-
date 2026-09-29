#include "anomaly.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace {

double clamp01(double x) {
    if (x < 0.0) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
}

std::optional<Price> mid_of(const std::vector<PriceLevel>& bids,
                            const std::vector<PriceLevel>& asks) {
    if (bids.empty() || asks.empty()) return std::nullopt;
    return (bids.front().price + asks.front().price) / 2.0;
}

Quantity depth_of(const std::vector<PriceLevel>& v, size_t n) {
    Quantity sum = 0;
    for (size_t i = 0; i < v.size() && i < n; ++i) sum += v[i].total_qty;
    return sum;
}

const char* side_name(OrderSide s) { return s == OrderSide::BUY ? "BUY" : "SELL"; }

// Two decimals, as a price. std::to_string would report 100.300000, which is
// noise in a line a human is meant to read.
std::string price_str(Price p) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << p;
    return os.str();
}

// Weights. `large_order` and `rapid_cancellation` are necessary to any spoof
// but are also the daily routine of every market maker, so they carry little
// weight on their own. The repetition and price-movement rules are the ones
// that actually distinguish manipulation from liquidity provision.
constexpr double W_LARGE   = 0.20;
constexpr double W_RAPID   = 0.25;
constexpr double W_REPEAT  = 0.30;
constexpr double W_IMPACT  = 0.30;
constexpr double W_RATIO   = 0.15;
constexpr double W_FLOW    = 0.20;
constexpr double W_LAYER   = 0.15;

}  // namespace

SpoofDetector::SpoofDetector(AnomalyConfig cfg) : cfg_(cfg) {}

// --- Baselines ------------------------------------------------------------

void SpoofDetector::update_baselines(const OrderActivity& a) {
    if (a.action != OrderActivity::Action::PLACE || !a.applied) return;

    // Winsorised input: the mean learns the bulk of the distribution, not its
    // tail. Without this the detector normalises a repeating spoof, and the
    // order that has done it six times is the one that finally looks ordinary.
    //
    // Only once the baseline is warm. During warmup the mean is still climbing
    // from zero, and clipping at 4x a mean that is itself still wrong ratchets
    // it upward without bound: each order moves the mean by 3 * alpha * mean,
    // so forty warmup orders of 20 lots left the baseline believing the typical
    // order was 267. Warmup must see the real data, or it learns a fiction and
    // then judges every genuine outlier against it.
    double x = static_cast<double>(a.qty);
    if (size_n_ >= cfg_.size_baseline_warmup && cfg_.baseline_winsor > 0.0) {
        const double cap = std::max(size_mean_, 1.0) * cfg_.baseline_winsor;
        x = std::min(x, cap);
    }

    // Exponentially weighted mean and variance (Welford form), but with the
    // warmup run at alpha = 1/n so it is a true running mean. Starting the EW at
    // a slow alpha instead would leave the mean at a fraction of the truth when
    // warmup ended, and every z-score after that would be measured against a
    // baseline that had not finished forming.
    const double alpha = (size_n_ < cfg_.size_baseline_warmup)
                             ? (size_n_ == 0 ? 1.0 : 1.0 / (static_cast<double>(size_n_) + 1.0))
                             : cfg_.baseline_alpha;

    const double delta = x - size_mean_;
    size_mean_ += alpha * delta;
    size_var_ = (1.0 - alpha) * (size_var_ + alpha * delta * delta);
    ++size_n_;
}

// --- Book context ---------------------------------------------------------

namespace {

struct PlacementContext {
    std::optional<Price> mid;
    double   spread = 0.0;
    Quantity depth = 0;
    double   ticks_from_mid = 0.0;
    bool     crossed_touch = false;
};

PlacementContext context_for(const OrderActivity& a, const AnomalyConfig& cfg) {
    PlacementContext c;
    // The placement context is the market as it was *before* the order arrived.
    // Falling back to the post-operation mid is only correct for a resting
    // order, and the two are indistinguishable here, so the pre-op value is
    // always preferred when the pipeline supplied one.
    c.mid   = a.prev_mid ? a.prev_mid : mid_of(a.bids, a.asks);
    c.depth = depth_of(a.bids, cfg.imbalance_depth) +
              depth_of(a.asks, cfg.imbalance_depth);
    if (c.mid && cfg.tick_size > 0.0) {
        c.ticks_from_mid = std::fabs(a.price - *c.mid) / cfg.tick_size;
    }
    if (!a.bids.empty() && !a.asks.empty()) {
        c.spread = a.asks.front().price - a.bids.front().price;
    }

    // Crossed on arrival means the order would have traded against the touch, so
    // leaving it unfilled was a decision and not a consequence of the book. Read
    // from the post-operation book: what matters is whether the resting offer
    // would have hit, not what the book looked like beforehand.
    if (a.side == OrderSide::SELL) {
        c.crossed_touch = !a.bids.empty() && a.price <= a.bids.front().price;
    } else {
        c.crossed_touch = !a.asks.empty() && a.price >= a.asks.front().price;
    }
    return c;
}

}  // namespace

// --- Lifecycle ------------------------------------------------------------

void SpoofDetector::track_placement(const OrderActivity& a) {
    if (a.type == OrderType::MARKET) return;   // never rests, so never spoofed

    OrderProfile p;
    p.id           = a.order_id;
    p.side         = a.side;
    p.type         = a.type;
    p.price        = a.price;
    p.qty          = a.qty;
    p.placed_ts_us = a.ts_us;
    p.placed_seq   = a.seq;

    const PlacementContext c = context_for(a, cfg_);
    p.mid_at_place     = c.mid;
    p.spread_at_place  = c.spread;
    p.depth_at_place   = c.depth;
    p.ticks_from_mid   = c.ticks_from_mid;
    p.crossed_touch    = c.crossed_touch;

    // "Large" is only claimed once the baseline has enough samples behind it to
    // support the claim. Before that, the order is unremarkable by default.
    if (size_n_ >= cfg_.size_baseline_warmup) {
        // The denominator is floored at a fraction of the mean. A perfectly
        // steady stream -- 20 lots, 20 lots, 20 lots -- has essentially zero
        // variance, and letting the z-score divide by that produced z = 0 for
        // everything, so a 900-lot order in a stream of 20s came out as
        // unremarkable. The floor keeps the score finite and still enormous,
        // and encodes the floor directly: with no variance to measure, being
        // several times the typical size is what counts.
        const double sd    = std::sqrt(std::max(size_var_, 0.0));
        const double floor_ = std::max(size_mean_, 1.0) * 0.25;
        p.size_zscore = (static_cast<double>(a.qty) - size_mean_) /
                        std::max(sd, floor_);
        p.large = a.qty >= cfg_.large_min_qty && p.size_zscore >= cfg_.large_zscore;
    }
    if (p.large) ++orders_large_;

    working_[p.id] = p;
    working_fifo_.push_back({a.ts_us, p.id});
    working_pos_[p.id] = std::prev(working_fifo_.end());
    ++orders_seen_;

    // Bound the map. Dropping the oldest working order is the right loss: a
    // resting order is a small memory cost, and a missed cancellation is a
    // missed signal, not a wrong one.
    while (working_.size() > cfg_.max_tracked && !working_fifo_.empty()) {
        const OrderId old = working_fifo_.front().second;
        const auto pos = working_pos_.find(old);
        if (pos != working_pos_.end()) {
            working_fifo_.erase(pos->second);
            working_pos_.erase(pos);
        } else {
            working_fifo_.pop_front();
        }
        working_.erase(old);
    }
}

void SpoofDetector::end_order(OrderId id, const OrderActivity& a,
                              OrderStatus status) {
    auto it = working_.find(id);
    if (it == working_.end()) return;

    OrderProfile& p = it->second;
    p.ended       = true;
    p.end_status  = status;
    p.ended_ts_us = a.ts_us;
    // Guard against a clock that steps backwards, which would otherwise produce
    // a negative lifetime and then a "rapid" cancellation that never happened.
    p.lifetime_us = a.ts_us > p.placed_ts_us
                        ? static_cast<double>(a.ts_us - p.placed_ts_us)
                        : 0.0;
    p.mid_at_end = mid_of(a.bids, a.asks);

    if (p.mid_at_place && p.mid_at_end && cfg_.tick_size > 0.0) {
        // Signed so that positive always means "the price moved the way this
        // placer wanted". A large sell wants the price down; a large buy wants
        // it up. Getting this backwards would flag every seller as a spoofer.
        const double down = *p.mid_at_place - *p.mid_at_end;
        p.mid_move_favor = (p.side == OrderSide::SELL) ? down : -down;
    }

    lifetime_sum_us_ += p.lifetime_us;
    ++lifetime_n_;
    if (p.large) {
        large_lifetime_sum_us_ += p.lifetime_us;
        if (status == OrderStatus::CANCELLED && p.lifetime_us <= cfg_.rapid_cancel_us) {
            ++rapid_cancels_;
        }
    }

    // Repetition is judged on the cluster, then the cluster's back-inserted
    // orders this one, because the signal only exists in the aggregate.
    std::vector<OrderId> cluster_ids;
    if (p.large && status == OrderStatus::CANCELLED &&
        p.lifetime_us <= cfg_.rapid_cancel_us) {
        const ClusterKey key = cluster_key_for(p);
        std::deque<std::pair<uint64_t, OrderId>>& d = clusters_[key];
        const uint64_t cutoff = a.ts_us > cfg_.cluster_window_us
                                    ? a.ts_us - cfg_.cluster_window_us : 0;
        while (!d.empty() && d.front().first < cutoff) d.pop_front();
        d.push_back({a.ts_us, p.id});
        for (const auto& e : d) cluster_ids.push_back(e.second);
    }

    const std::vector<AnomalySignal> signals = evaluate(p, a.ts_us);
    last_signals_ = signals;
    maybe_alert(p, a.ts_us, signals, cluster_ids);

    closed_.push_back(p);
    if (closed_.size() > cfg_.max_tracked) closed_.pop_front();

    // Drop the FIFO entry too, so a completed order costs nothing to forget.
    const auto pos = working_pos_.find(id);
    if (pos != working_pos_.end()) {
        working_fifo_.erase(pos->second);
        working_pos_.erase(pos);
    }
    working_.erase(it);
}

void SpoofDetector::apply_fills(const OrderActivity& a) {
    for (const auto& f : a.fills) {
        // Both sides of the trade can be an order this detector is tracking.
        // The passive one was lifted off the book; the aggressor may be the
        // order this very operation placed, which is how an order that filled
        // on arrival gets its profile closed instead of leaked.
        for (OrderId id : {f.passive_id, f.aggressor_id}) {
            auto it = working_.find(id);
            if (it == working_.end()) continue;

            it->second.filled_qty += f.qty;
            if (it->second.filled_qty >= it->second.qty) {
                // Fully filled, so the order did its job. It is still scored --
                // a large order that lifts the book is worth knowing about --
                // but the cancellation-only rules cannot fire on it.
                end_order(id, a, OrderStatus::FILLED);
            }
        }
    }
}

void SpoofDetector::evict(uint64_t now_us) {
    const uint64_t cut = now_us > cfg_.window_us ? now_us - cfg_.window_us : 0;
    while (!window_placements_.empty() && window_placements_.front().ts_us < cut) {
        window_placements_.pop_front();
    }
    while (!window_cancels_.empty() && window_cancels_.front().ts_us < cut) {
        window_cancels_.pop_front();
    }
    while (!window_flow_.empty() && window_flow_.front().first < cut) {
        window_flow_.pop_front();
    }

    // Clusters are keyed by price band, so without this the map gains an entry
    // for every price anyone ever touched and never gives any of them back.
    const uint64_t ccut =
        now_us > cfg_.cluster_window_us ? now_us - cfg_.cluster_window_us : 0;
    for (auto it = clusters_.begin(); it != clusters_.end();) {
        if (it->second.empty() || it->second.back().first < ccut) {
            it = clusters_.erase(it);
        } else {
            ++it;
        }
    }
}

// --- Entry point ----------------------------------------------------------

void SpoofDetector::observe(const OrderActivity& a) {
    evict(a.ts_us);

    switch (a.action) {
        case OrderActivity::Action::PLACE: {
            if (!a.applied) return;      // a refused order never existed
            // Tracked before the baseline moves, so an order is always judged
            // against the flow that existed without it. Judging it against a
            // baseline it has already helped write is circular.
            if (a.type == OrderType::LIMIT) track_placement(a);
            update_baselines(a);
            window_placements_.push_back({a.ts_us, a.qty});
            const double signed_qty = a.side == OrderSide::BUY
                                          ? static_cast<double>(a.qty)
                                          : -static_cast<double>(a.qty);
            window_flow_.push_back({a.ts_us, signed_qty});
            // A market order never rests, so it can never be spoofed: there is
            // no cancellation to make and no order to leave hanging.
            break;
        }

        case OrderActivity::Action::CANCEL: {
            if (!a.applied) return;
            window_cancels_.push_back({a.ts_us, a.qty});
            end_order(a.order_id, a, OrderStatus::CANCELLED);
            break;
        }

        case OrderActivity::Action::MODIFY:
            return;   // a modify is neither a placement nor a cancellation
    }

    apply_fills(a);
}

// --- Rules ----------------------------------------------------------------

SpoofDetector::ClusterKey SpoofDetector::cluster_key_for(const OrderProfile& p) const {
    // Bucket by price band rather than exact price: the same trick repeated at
    // 105.00 and 105.01 is one pattern, not two unrelated events.
    const double band = cfg_.tick_size > 0.0 ? cfg_.cluster_band_ticks * cfg_.tick_size
                                              : 0.01;
    const int64_t bucket = band > 0.0 ? static_cast<int64_t>(std::llround(p.price / band))
                                      : static_cast<int64_t>(p.price);
    return ClusterKey{p.side, bucket};
}

std::vector<AnomalySignal> SpoofDetector::evaluate(const OrderProfile& p,
                                                   uint64_t now_us) const {
    std::vector<AnomalySignal> sigs;
    auto add = [&](const char* name, double strength, double weight,
                   std::string detail, bool context = false) {
        strength = clamp01(strength);
        if (strength <= 0.0) return;
        sigs.push_back(AnomalySignal{name, strength, strength * weight, context,
                                     std::move(detail)});
    };

    // 1. Large order. The floor here is the statistical one, applied at
    //    placement; a z-score alone would flag the first order of a session.
    if (p.large) {
        add("large_order", (p.size_zscore - cfg_.large_zscore) / cfg_.large_zscore +
                               0.5,
            W_LARGE,
            "qty " + std::to_string(p.qty) + " is " +
                std::to_string(static_cast<int>(p.size_zscore)) +
                " sd above recent placements (mean " +
                std::to_string(static_cast<int>(size_mean_)) + ")");
    }

    const bool cancelled_unfilled =
        p.end_status == OrderStatus::CANCELLED && p.completed_unfilled();

    // 2. Rapid cancellation. A large order withdrawn inside the threshold, and
    //    never traded: the core single-order signature.
    if (p.large && cancelled_unfilled && cfg_.rapid_cancel_us > 0) {
        add("rapid_cancellation", 1.0 - p.lifetime_us / static_cast<double>(cfg_.rapid_cancel_us),
            W_RAPID,
            "withdrew " + std::to_string(p.qty) + " after " +
                std::to_string(static_cast<int>(p.lifetime_us / 1000)) + "ms unfilled");
    }

    // 3. Repetition. Only spoof-shaped orders (large, fast, unfilled) join a
    //    cluster, so a count here is a count of suspicious behaviour, not of
    //    ordinary size.
    if (!clusters_.empty()) {
        const ClusterKey key = cluster_key_for(p);
        auto it = clusters_.find(key);
        if (it != clusters_.end()) {
            const uint64_t cut = now_us > cfg_.cluster_window_us
                                     ? now_us - cfg_.cluster_window_us : 0;
            size_t n = 0;
            for (const auto& e : it->second) {
                if (e.first >= cut) ++n;
            }
            if (n >= cfg_.repeat_min) {
                const double span = std::max<double>(
                    1.0, static_cast<double>(cfg_.repeat_max - cfg_.repeat_min + 1));
                add("repeated_placement", (n - cfg_.repeat_min + 1.0) / span, W_REPEAT,
                    std::to_string(n) + " large " + side_name(p.side) +
                        " orders near " + price_str(p.price) + " within " +
                        std::to_string(cfg_.cluster_window_us / 1000000) + "s");
            }
        }
    }

    // 4. Price manipulation. The mid moved in the placer's favour while the
    //    order was up, which is the part that separates intent from a hedge.
    if (p.large && cfg_.tick_size > 0.0) {
        const double ticks = p.mid_move_favor / cfg_.tick_size;
        if (ticks >= cfg_.impact_min_ticks) {
            const double span = std::max(0.5, cfg_.impact_max_ticks - cfg_.impact_min_ticks);
            add("price_manipulation", (ticks - cfg_.impact_min_ticks) / span, W_IMPACT,
                std::string(side_name(p.side)) + " order up while mid moved " +
                    std::to_string(static_cast<int>(ticks)) + " tick(s) its way");
        }
    }

    // 5. Cancellation-to-placement ratio, as *excess over a market maker's
    //    baseline*. The raw ratio is near 0.95 for legitimate liquidity
    //    provision, so scoring it directly would flag the whole industry.
    //    This is a property of the flow, not of this order, so it is marked
    //    context: it modulates the score instead of counting as evidence.
    if (!window_placements_.empty() && cfg_.cancel_ratio_span > 0.0) {
        const double ratio = cancel_to_placement_ratio();
        add("cancel_ratio", (ratio - cfg_.cancel_ratio_baseline) / cfg_.cancel_ratio_span,
            W_RATIO,
            "cancel/place " + std::to_string(static_cast<int>(ratio * 100)) +
                "% vs baseline " +
                std::to_string(static_cast<int>(cfg_.cancel_ratio_baseline * 100)) + "%",
            true);
    }

    // 6. Flow concentration. One order carrying most of the window's flow is a
    //    different claim from one order being large -- but like the ratio above
    //    it describes the window, so it is context rather than evidence.
    if (!window_flow_.empty()) {
        double total_abs = 0.0;
        for (const auto& f : window_flow_) total_abs += std::fabs(f.second);
        if (total_abs > 0.0) {
            const double share = std::fabs(static_cast<double>(p.qty)) / total_abs;
            if (share > cfg_.flow_share_min) {
                add("flow_concentration",
                    (share - cfg_.flow_share_min) / std::max(1e-9, 1.0 - cfg_.flow_share_min),
                    W_FLOW,
                    std::to_string(static_cast<int>(share * 100)) +
                        "% of recent order flow",
                    true);
            }
        }
    }

    // 7. Layering. A large order parked well away from the touch, then pulled,
    //    is the footprint of building a wall that is never meant to be hit.
    if (p.large && cancelled_unfilled && cfg_.layering_min_ticks > 0.0 &&
        p.ticks_from_mid >= cfg_.layering_min_ticks) {
        add("layering", (p.ticks_from_mid - cfg_.layering_min_ticks) /
                            std::max(1.0, p.ticks_from_mid),
            W_LAYER,
                std::to_string(static_cast<int>(p.ticks_from_mid)) +
                    " ticks from the mid, withdrawn unfilled");
    }

    return sigs;
}

void SpoofDetector::maybe_alert(const OrderProfile& p, uint64_t ts_us,
                                const std::vector<AnomalySignal>& signals,
                                const std::vector<OrderId>& cluster) {
    if (signals.empty()) return;

    // Evidence and context are counted separately. Both matter, but they are
    // not the same kind of claim: `large_order` and `repeated_placement` are
    // facts about *this order*, while a high cancel ratio and a concentrated
    // flow describe the window it happened in. Letting context count toward
    // the multiplicity cap would mean an order inherits suspicion from its
    // neighbours -- a benign order posted in a busy book would be capped out by
    // two series-level facts, and the cap that exists to prevent false
    // positives would itself manufacture them.
    double residual = 1.0;
    double context_gain = 1.0;
    size_t evidence = 0;
    for (const auto& s : signals) {
        if (s.context) {
            context_gain *= 1.0 + clamp01(s.weight);
        } else {
            residual *= (1.0 - clamp01(s.weight));
            ++evidence;
        }
    }
    const double combined = (1.0 - residual) * context_gain;

    // The multiplicity cap is the whole false-positive story. One signal alone
    // can reach at most 100/min_signals, so the single big fast cancel that
    // every market maker produces all day cannot reach the alert threshold,
    // however extreme it was.
    const double count_factor =
        clamp01(static_cast<double>(evidence) / std::max(1.0, cfg_.min_signals));
    const double score = 100.0 * combined * count_factor;

    if (evidence == 0) return;   // context alone is not an order's fault

    // A pattern means repetition or price movement: evidence that the order did
    // something rather than merely existing.
    bool pattern = false;
    for (const auto& s : signals) {
        if (s.name == "repeated_placement" || s.name == "price_manipulation") {
            pattern = true;
        }
    }

    if (score < cfg_.alert_score) return;
    if (cfg_.require_pattern && !pattern) return;

    Alert al;
    al.seq      = p.placed_seq;
    al.ts_us    = ts_us;
    al.score    = score;
    al.order_id = p.id;
    al.pattern  = pattern;
    al.signals  = signals;
    al.related_orders = cluster;
    al.summary = std::string(side_name(p.side)) + " " +
                 std::to_string(p.qty) + "@" + price_str(p.price) + ", " +
                 std::to_string(static_cast<int>(p.lifetime_us / 1000)) + "ms, " +
                 (p.completed_unfilled() ? "never filled" : "partially filled") + ", " +
                 std::to_string(evidence) + " of 5 order rules";
    alerts_.push_back(std::move(al));
}

// --- Accessors ------------------------------------------------------------

const OrderProfile* SpoofDetector::profile(OrderId id) const {
    auto it = working_.find(id);
    if (it != working_.end()) return &it->second;
    for (auto r = closed_.rbegin(); r != closed_.rend(); ++r) {
        if (r->id == id) return &*r;
    }
    return nullptr;
}

double SpoofDetector::cancel_to_placement_ratio() const {
    if (window_placements_.empty()) return 0.0;
    return static_cast<double>(window_cancels_.size()) /
           static_cast<double>(window_placements_.size());
}

double SpoofDetector::mean_order_lifetime_us() const {
    return lifetime_n_ ? lifetime_sum_us_ / static_cast<double>(lifetime_n_) : 0.0;
}

double SpoofDetector::mean_large_lifetime_us() const {
    return orders_large_ ? large_lifetime_sum_us_ / static_cast<double>(orders_large_)
                         : 0.0;
}

double SpoofDetector::placements_per_second() const {
    if (window_placements_.size() < 2) return 0.0;
    const uint64_t span = window_placements_.back().ts_us - window_placements_.front().ts_us;
    if (span == 0) return 0.0;
    return static_cast<double>(window_placements_.size()) /
           (static_cast<double>(span) / 1e6);
}

double SpoofDetector::cancellations_per_second() const {
    if (window_cancels_.size() < 2) return 0.0;
    const uint64_t span = window_cancels_.back().ts_us - window_cancels_.front().ts_us;
    if (span == 0) return 0.0;
    return static_cast<double>(window_cancels_.size()) /
           (static_cast<double>(span) / 1e6);
}

std::string SpoofDetector::report() const {
    std::ostringstream os;
    os << "orders tracked  : " << orders_seen() << " (" << orders_large()
       << " large, " << rapid_cancels() << " rapid cancel of a large order)\n";
    os << "cancel / place  : " << cancel_to_placement_ratio() * 100.0 << "%  ("
       << std::fixed << std::setprecision(1) << placements_per_second() << "/s placed, "
       << cancellations_per_second() << "/s cancelled)\n";
    os << "lifetime        : mean " << static_cast<int>(mean_order_lifetime_us() / 1000)
       << "ms, large orders " << static_cast<int>(mean_large_lifetime_us() / 1000) << "ms\n";
    os << "alerts          : " << alerts_raised() << " of " << lifetime_n_
       << " closed orders\n";
    os << std::fixed << std::setprecision(2);

    if (!alerts_.empty()) {
        os << "\n-- alerts --------------------------------------------------------\n";
        for (const auto& a : alerts_) {
            os << "  [" << std::setw(5) << a.score << "] order " << a.order_id << "  "
               << a.summary << "\n";
            for (const auto& s : a.signals) {
                os << "           " << std::left << std::setw(20) << s.name
                   << std::right << std::setw(4) << static_cast<int>(s.strength * 100)
                   << "%  +" << s.weight << (s.context ? "  (context)  " : "             ")
                   << s.detail << "\n";
            }
            if (a.related_orders.size() > 1) {
                os << "           cluster:";
                for (OrderId id : a.related_orders) os << " " << id;
                os << "\n";
            }
        }
    }
    return os.str();
}
