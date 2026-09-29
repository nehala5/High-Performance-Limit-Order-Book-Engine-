#include "pipeline.h"

#include <algorithm>
#include <sstream>

std::string Analytics::to_string() const {
    std::ostringstream os;
    os << "feed lines      : " << lines_parsed << " parsed, " << lines_malformed
       << " malformed, " << lines_ignored << " ignored\n";
    os << "orders          : " << adds << " add (" << adds_accepted << " accepted, "
       << adds_rejected << " rejected)\n"
       << "                  " << cancels << " cancel (" << cancel_ok << " ok, "
       << cancel_refused << " refused)\n"
       << "                  " << modifies << " modify (" << modify_ok << " ok, "
       << modify_refused << " refused)\n";
    os << "trade prints    : " << trade_prints << " (excluded from book stats)\n";
    os << "executions      : " << trades << "\n";
    os << "volume          : " << total_volume << " (buy " << buy_volume << " / sell "
       << sell_volume << ")\n";
    if (has_trades) {
        os << "vwap            : " << vwap() << "\n";
        os << "range           : " << low << " - " << high << "\n";
        os << "largest trade   : " << largest_trade << "\n";
    }
    os << "fill rate       : " << fill_rate() * 100.0 << "%  (" << filled_qty << " of "
       << submitted_qty << " submitted)\n";
    os << "cancel rate     : " << cancel_rate() * 100.0 << "%\n";
    if (snapshots_with_spread) {
        os << "spread          : mean " << mean_spread << ", range " << min_spread << " - "
           << max_spread << "  (" << snapshots_with_spread << "/" << snapshots
           << " snapshots)\n";
    }
    os << "snapshots       : " << snapshots << "\n";
    return os.str();
}

Pipeline::Pipeline(OrderBook& book, PipelineConfig cfg) : book_(book), cfg_(cfg) {}

void Pipeline::take_snapshot(uint64_t exchange_ts) {
    SnapshotRecord rec;
    rec.seq            = book_.sequence();
    rec.exchange_ts    = exchange_ts;
    rec.recorded_at_us = epoch_micros_now();

    L2Snapshot d = book_.depth(cfg_.snapshot_depth);
    rec.best_bid  = d.best_bid;
    rec.best_ask  = d.best_ask;
    rec.mid_price = d.mid_price;
    rec.spread    = d.spread;
    rec.bids      = std::move(d.bids);
    rec.asks      = std::move(d.asks);

    snapshots_.push_back(rec);
    last_snapshot_ = rec;
}

void Pipeline::maybe_snapshot(bool applied, FeedEventType type, uint64_t exchange_ts) {
    if (!applied) return;
    if (cfg_.snapshot_every && ++ops_since_snapshot_ >= cfg_.snapshot_every) {
        ops_since_snapshot_ = 0;
        take_snapshot(exchange_ts);
    }
    if (cfg_.snapshot_on_add && type == FeedEventType::ADD) {
        take_snapshot(exchange_ts);
    }
}

void Pipeline::emit_observation(const FeedEvent& ev, const Outcome& out,
                                const std::vector<Trade>& executed) {
    if (!analyzer_) return;

    Observation o;
    o.seq  = out.seq;
    // The feed's own clock when the line carried one, so a whole series shares
    // one time base. Falling back to the wall clock mid-feed would mix bases
    // and corrupt every rate; the fallback is for feeds that never timestamp.
    o.ts_us = ev.exchange_ts ? (ev.exchange_ts / 1000) : epoch_micros_now();

    L2Snapshot d = book_.depth(cfg_.observe_depth);
    o.bids = std::move(d.bids);
    o.asks = std::move(d.asks);

    o.is_add    = (ev.type == FeedEventType::ADD);
    o.is_cancel = (ev.type == FeedEventType::CANCEL);
    o.is_modify = (ev.type == FeedEventType::MODIFY);
    o.applied   = out.applied;
    o.order_qty = o.is_add ? (out.filled_qty + out.leaves_qty) : 0;
    o.leaves_qty = out.leaves_qty;

    o.trade_count = static_cast<uint32_t>(executed.size());
    for (const auto& t : executed) {
        o.trade_qty += t.qty;
        if (t.side == OrderSide::BUY) o.buy_qty += t.qty;
        else                         o.sell_qty += t.qty;
        o.trade_notional += t.price * static_cast<double>(t.qty);
    }

    analyzer_->observe(o);
}

void Pipeline::emit_activity(const FeedEvent& ev, const Outcome& out,
                             const std::vector<Trade>& executed,
                             const std::optional<Price>& pre_mid) {
    if (!detector_) return;

    OrderActivity a;
    a.seq    = out.seq;
    a.ts_us  = ev.exchange_ts ? (ev.exchange_ts / 1000) : epoch_micros_now();
    a.applied = out.applied;

    switch (ev.type) {
        case FeedEventType::ADD:    a.action = OrderActivity::Action::PLACE;  break;
        case FeedEventType::CANCEL: a.action = OrderActivity::Action::CANCEL; break;
        case FeedEventType::MODIFY: a.action = OrderActivity::Action::MODIFY; break;
        default: return;   // a trade print changes nothing the detector can judge
    }

    a.order_id = out.order_id;
    a.side     = ev.side;
    a.type     = ev.order_type;
    a.price    = ev.price;
    a.qty      = ev.qty;
    a.status   = out.status;

    // For a cancel, `out.leaves_qty` is the size actually withdrawn, which is
    // what the cancellation-volume feature is measured against.
    if (a.action == OrderActivity::Action::CANCEL) a.qty = out.leaves_qty;

    // The market as it was before this order arrived. Both accessors are O(1),
    // and without them the detector would measure an aggressive order against
    // a book its own size had already moved.
    a.prev_mid = pre_mid;

    L2Snapshot d = book_.depth(cfg_.observe_depth);
    a.bids = std::move(d.bids);
    a.asks = std::move(d.asks);

    a.fills.reserve(executed.size());
    for (const auto& t : executed) {
        a.fills.push_back(OrderActivity::Fill{t.passive_id, t.aggressor_id, t.qty,
                                              t.price, t.side});
    }

    detector_->observe(a);
}

Pipeline::Outcome Pipeline::submit_line(std::string_view line) {
    FeedParseResult pr = FeedParser::parse(line, 0);

    if (!pr.ok) {
        ++lines_malformed_;
        Outcome out;
        out.error = pr.error;
        return out;
    }

    return apply(pr.event);
}

Pipeline::Outcome Pipeline::apply(const FeedEvent& ev) {
    Outcome out;
    out.type     = ev.type;
    out.order_id = ev.order_id;

    if (ev.type == FeedEventType::NONE) {
        ++lines_ignored_;
        out.parsed  = true;
        out.ignored = true;
        return out;
    }

    if (ev.type == FeedEventType::UNKNOWN) {
        ++lines_ignored_;
        out.parsed  = true;
        out.ignored = true;
        out.error   = "unknown verb '" + ev.verb + "'";
        return out;
    }

    out.parsed = true;
    // Counted here rather than at the call sites so the tally is right whether
    // an event arrives from submit_line, submit_text or apply() directly.
    ++lines_parsed_;
    const SeqNum before = book_.sequence();
    // The pre-operation mid, for the detector's placement context. Taken here
    // because the book is about to change, and `emit_activity` runs after.
    const std::optional<Price> pre_mid = book_.mid_price();

    OrderEventRecord rec;
    rec.exchange_ts    = ev.exchange_ts;
    rec.recorded_at_us = epoch_micros_now();
    rec.raw            = ev.raw;
    rec.order_id       = ev.order_id;

    if (ev.type == FeedEventType::ADD) {
        ExecutionReport r = (ev.order_type == OrderType::MARKET)
                                ? book_.add_market_order(ev.side, ev.qty, ev.order_id)
                                : book_.add_limit_order(ev.side, ev.price, ev.qty, ev.order_id);

        out.applied    = r.accepted();
        out.seq        = r.seq;
        out.order_id   = r.order_id;
        out.avg_price  = r.avg_price();
        out.filled_qty = r.filled_qty;
        out.leaves_qty = r.leaves_qty;
        out.status     = r.status;
        if (r.rejected()) out.error = r.reject_reason();

        rec.applied       = r.accepted();
        rec.type          = FeedEventType::ADD;
        rec.order_id      = r.order_id;
        rec.final_status  = r.status;
        rec.side          = ev.side;
        rec.order_type    = ev.order_type;
        rec.price         = (ev.order_type == OrderType::MARKET) ? 0.0 : ev.price;
        rec.requested_qty = r.requested_qty;
        rec.filled_qty    = r.filled_qty;
        rec.leaves_qty    = r.leaves_qty;
        rec.reject_code   = r.reject_code;

    } else if (ev.type == FeedEventType::CANCEL) {
        // The engine reports a refusal by return value, not by status: a cancel
        // aimed at a FILLED order fails while the order's status stays FILLED.
        // Taking `applied` from the return value is the only way to tell those
        // apart.
        out.applied = book_.cancel_order(ev.order_id);
        out.seq     = book_.sequence();
        if (!out.applied) out.error = "cancel refused";

        rec.applied = out.applied;
        rec.type    = FeedEventType::CANCEL;
        if (const Order* o = book_.get_order(ev.order_id)) {
            rec.final_status  = o->status;
            rec.side          = o->side;
            rec.order_type    = o->type;
            rec.price         = o->price;
            rec.requested_qty = o->qty;
            rec.filled_qty    = o->filled_qty;
            rec.leaves_qty    = o->remaining();
            // The size a cancel withdrew, which is what the volume-based
            // cancellation rate is measured against.
            out.filled_qty = o->filled_qty;
            out.leaves_qty = o->remaining();
        } else {
            rec.final_status = OrderStatus::REJECTED;
        }
        // Reported whether or not the operation succeeded: a refused cancel on
        // a FILLED order should say FILLED, which is the reason it was refused.
        out.status = rec.final_status;

    } else if (ev.type == FeedEventType::MODIFY) {
        out.applied = book_.modify_order(ev.order_id, ev.new_qty,
                                         ev.has_new_price
                                             ? std::optional<Price>(ev.new_price)
                                             : std::nullopt);
        out.seq = book_.sequence();
        if (!out.applied) out.error = "modify refused";

        rec.applied = out.applied;
        rec.type    = FeedEventType::MODIFY;
        if (const Order* o = book_.get_order(ev.order_id)) {
            rec.final_status  = o->status;
            rec.side          = o->side;
            rec.order_type    = o->type;
            rec.price         = o->price;
            rec.requested_qty = o->qty;
            rec.filled_qty    = o->filled_qty;
            rec.leaves_qty    = o->remaining();
        } else {
            rec.final_status = OrderStatus::REJECTED;
        }
        out.status = rec.final_status;

    } else if (ev.type == FeedEventType::TRADE_PRINT) {
        // A consolidated tape print. Recorded, but never fed to the book: the
        // engine's own executions are the authority, and a print is something
        // to reconcile them against.
        TradeRecord tr;
        tr.seq            = book_.sequence();
        tr.aggressor_id   = 0;
        tr.passive_id     = 0;
        tr.side           = OrderSide::BUY;
        tr.price          = ev.price;
        tr.qty            = ev.qty;
        tr.exchange_ts    = ev.exchange_ts;
        tr.recorded_at_us = epoch_micros_now();
        tr.from_print     = true;
        trade_events_.push_back(tr);

        out.applied = true;
        out.seq     = tr.seq;
        rec.type    = FeedEventType::TRADE_PRINT;
        rec.applied = true;
        rec.price   = ev.price;
        rec.requested_qty = ev.qty;

        order_events_.push_back(rec);
        maybe_snapshot(true, ev.type, ev.exchange_ts);
        return out;
    }

    rec.seq = out.seq;
    order_events_.push_back(rec);

    // Whatever the engine executed as a consequence of this line, including
    // trades caused by a modify that crossed.
    const std::vector<Trade> executed = book_.trades_since(before);
    const uint64_t at_us = epoch_micros_now();
    for (const auto& t : executed) {
        TradeRecord tr;
        tr.trade_id      = t.trade_id;
        tr.seq           = t.seq;
        tr.aggressor_id  = t.aggressor_id;
        tr.passive_id    = t.passive_id;
        tr.side          = t.side;
        tr.price         = t.price;
        tr.qty           = t.qty;
        tr.exchange_ts   = ev.exchange_ts;
        tr.recorded_at_us = at_us;
        tr.from_print    = false;
        trade_events_.push_back(tr);
    }
    out.trades = executed;

    // Both consumers are fed from the same place, after the executions are
    // known. They disagree about what matters -- the analyzer measures the
    // book, the detector measures the orders -- so they cannot be merged.
    if (out.applied) {
        emit_observation(ev, out, executed);
        emit_activity(ev, out, executed, pre_mid);
    }

    maybe_snapshot(out.applied, ev.type, ev.exchange_ts);
    return out;
}

std::vector<Pipeline::Outcome> Pipeline::submit_text(std::string_view text) {
    std::vector<Outcome> out;
    size_t line_no = 0;
    size_t start = 0;

    // `start < text.size()` rather than `<=`, so a feed ending in a newline
    // does not contribute a phantom empty line -- the getline convention.
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        std::string_view line =
            (nl == std::string_view::npos) ? text.substr(start) : text.substr(start, nl - start);
        ++line_no;

        FeedParseResult pr = FeedParser::parse(line, line_no);
        if (!pr.ok) {
            ++lines_malformed_;
            Outcome o;
            o.error = "line " + std::to_string(line_no) + ": " + pr.error;
            out.push_back(std::move(o));
        } else {
            Outcome o = apply(pr.event);
            // Blank and comment lines are counted as ignored but produce no
            // outcome, so the returned vector is one entry per real line.
            if (o.type != FeedEventType::NONE) out.push_back(std::move(o));
        }

        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return out;
}

Analytics Pipeline::analytics() const {
    Analytics a;
    a.lines_parsed    = lines_parsed_;
    a.lines_malformed = lines_malformed_;
    a.lines_ignored   = lines_ignored_;

    for (const auto& rec : order_events_) {
        switch (rec.type) {
            case FeedEventType::ADD:
                ++a.adds;
                a.submitted_qty += rec.requested_qty;
                a.filled_qty    += rec.filled_qty;
                if (rec.applied) ++a.adds_accepted;
                else            ++a.adds_rejected;
                break;

            case FeedEventType::CANCEL:
                ++a.cancels;
                if (rec.applied) {
                    ++a.cancel_ok;
                    a.cancelled_qty += rec.leaves_qty;
                } else {
                    ++a.cancel_refused;
                }
                break;

            case FeedEventType::MODIFY:
                ++a.modifies;
                if (rec.applied) ++a.modify_ok;
                else            ++a.modify_refused;
                break;

            case FeedEventType::TRADE_PRINT:
                ++a.trade_prints;
                break;

            default:
                break;
        }
    }

    // Engine executions only. A feed print must not inflate the book's numbers.
    for (const auto& t : trade_events_) {
        if (t.from_print) continue;
        ++a.trades;
        a.total_volume    += t.qty;
        a.total_notional  += t.price * static_cast<double>(t.qty);
        a.volume_by_price[t.price] += t.qty;
        if (t.side == OrderSide::BUY) a.buy_volume += t.qty;
        else                         a.sell_volume += t.qty;
        if (t.qty > a.largest_trade) a.largest_trade = t.qty;

        if (!a.has_trades) {
            a.high = a.low = t.price;
            a.has_trades = true;
        } else {
            a.high = std::max(a.high, t.price);
            a.low  = std::min(a.low, t.price);
        }
    }

    a.snapshots = snapshots_.size();
    double sum = 0.0;
    for (const auto& s : snapshots_) {
        if (!s.spread) continue;
        ++a.snapshots_with_spread;
        sum += *s.spread;
        if (a.snapshots_with_spread == 1) {
            a.min_spread = a.max_spread = *s.spread;
        } else {
            a.min_spread = std::min(a.min_spread, *s.spread);
            a.max_spread = std::max(a.max_spread, *s.spread);
        }
    }
    a.mean_spread = a.snapshots_with_spread
                        ? sum / static_cast<double>(a.snapshots_with_spread)
                        : 0.0;

    return a;
}
