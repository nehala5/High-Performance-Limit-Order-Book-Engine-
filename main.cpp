#include "order_book.h"
#include "pipeline.h"
#include "microstructure.h"
#include <iostream>
#include <limits>
#include <sstream>

// A short session, exactly as a venue would hand it over: text, with venue
// timestamps and the occasional bad line. Nothing below is inserted by hand.
static void demo_pipeline() {
    std::cout << "\n\n=== 13. Event / Market Data Pipeline ===\n";
    std::cout << "   market data -> parser -> order events -> LOB -> trade events -> analytics\n\n";

    static const char* feed =
        "# AAPL, tick 0.01\n"
        "ADD SELL LIMIT 105.00 100 1 ts=1700000000000000000\n"
        "ADD SELL LIMIT 105.00  50 2 ts=1700000000100000000\n"
        "ADD SELL LIMIT 106.00 200 3 ts=1700000000200000000\n"
        "ADD BUY  LIMIT 105.00 120 4 ts=1700000000300000000\n"
        "CANCEL 2                     ts=1700000000400000000\n"
        "ADD BUY  LIMIT 104.00  40 5 ts=1700000000500000000\n"
        "MODIFY 4 130                         # grow, keeps priority\n"
        "ADD SELL LIMIT 104.00  60 6 ts=1700000000700000000\n"
        "ADD BUY  MARKET  25 7         ts=1700000000800000000\n"
        "\n"
        "ADD SELL LIMIT oops 10 8      # malformed: reaches neither book nor log\n"
        "ADD BUY  LIMIT 105.00 5 1     # duplicate id: refused by the engine\n"
        "ADD SELL LIMIT 105.005 5 9    # off the 0.01 tick grid: refused\n"
        "TRADE 104.00 60 5 6 ts=1700000000900000000   # a tape print\n";

    OrderBook book("AAPL", 0.01);
    PipelineConfig cfg;
    cfg.snapshot_every = 4;
    Pipeline pipe(book, cfg);

    auto outcomes = pipe.submit_text(feed);

    std::cout << "-- per line --\n";
    for (const auto& o : outcomes) {
        if (!o.parsed) {
            std::cout << "  [malformed] " << o.error << "\n";
            continue;
        }
        if (o.ignored) {
            std::cout << "  [ignored]   " << o.error << "\n";
            continue;
        }
        std::cout << "  " << feed_type_name(o.type) << " id=" << o.order_id
                  << " seq=" << o.seq
                  << " status=" << status_name(o.status)
                  << " filled=" << o.filled_qty << " leaves=" << o.leaves_qty;
        if (o.avg_price)
            std::cout << " avg_px=" << std::fixed << std::setprecision(2) << *o.avg_price;
        if (!o.error.empty()) std::cout << "  (" << o.error << ")";
        std::cout << std::defaultfloat << "\n";
    }

    std::cout << "\n-- order events stored (" << pipe.order_events().size() << ") --\n";
    for (const auto& r : pipe.order_events()) {
        std::cout << "  seq=" << r.seq << " " << feed_type_name(r.type)
                  << " id=" << r.order_id
                  << " " << (r.side == OrderSide::BUY ? "BUY " : "SELL")
                  << " px=" << std::fixed << std::setprecision(3) << r.price
                  << " qty=" << r.requested_qty
                  << " filled=" << r.filled_qty
                  << " ts=" << r.exchange_ts
                  << " stored=" << r.recorded_at_us
                  << (r.applied ? "" : "  REFUSED") << std::defaultfloat << "\n";
    }

    std::cout << "\n-- trade events stored (" << pipe.trade_events().size() << ") --\n";
    for (const auto& t : pipe.trade_events()) {
        std::cout << "  " << std::fixed << std::setprecision(2) << t.price
                  << " x " << t.qty
                  << " " << (t.side == OrderSide::BUY ? "BUY " : "SELL")
                  << " aggressor=" << t.aggressor_id << " passive=" << t.passive_id
                  << " seq=" << t.seq
                  << (t.from_print ? "   (tape print, not engine-matched)" : "")
                  << std::defaultfloat << "\n";
    }

    std::cout << "\n-- book snapshot --\n";
    L2Snapshot d = book.depth(3);
    d.print();

    std::cout << "\n-- analytics --\n"
              << std::fixed << std::setprecision(2) << pipe.analytics().to_string()
              << std::defaultfloat;

    // The feed path and the replay path must agree, or reconstruction is a lie.
    OrderBook rebuilt = replay("AAPL", book.command_log().all(), 0.01);
    std::cout << "-- feed then replay --\n";
    std::cout << "  original seq=" << book.sequence()
              << "  replayed seq=" << rebuilt.sequence()
              << "  trades " << book.trades().size() << "/" << rebuilt.trades().size()
              << "  ->  " << (book.depth(50).bids.size() == rebuilt.depth(50).bids.size() &&
                              book.total_orders() == rebuilt.total_orders() &&
                              book.total_notional() == rebuilt.total_notional()
                                  ? "identical" : "DIVERGED")
              << "\n";

    std::string perr;
    std::cout << "  invariants: " << (book.validate(&perr) ? "OK" : "VIOLATED -> " + perr) << "\n";
}

// One fixed-width table cell, blank when the metric is undefined. A missing
// bid is not the same as a bid of zero, and the table should not pretend
// otherwise.
static std::string cell(const std::optional<double>& v, int width = 8) {
    if (!v) return std::string(static_cast<size_t>(width), ' ');
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << std::setw(width) << *v;
    return os.str();
}

// A short session with microstructure attached: a book that builds, sweeps,
// gets pulled, and re-quotes, one second at a time. The point is that none of
// the metrics below are computed by hand -- they fall out of the feed.
static void demo_microstructure() {
    std::cout << "\n\n=== 14. Market Microstructure Analytics ===\n";
    std::cout << "   one observation per book change, computed as the feed arrives\n\n";

    static const char* feed =
        "ADD SELL LIMIT 105.00 100 1 ts=1700000000000000000\n"
        "ADD SELL LIMIT 105.00  50 2 ts=1700000001000000000\n"
        "ADD SELL LIMIT 106.00 200 3 ts=1700000002000000000\n"
        "ADD BUY  LIMIT 104.00  40 4 ts=1700000003000000000\n"
        "ADD BUY  LIMIT 105.00 120 5 ts=1700000004000000000\n"   // sweeps 150
        "CANCEL 2                 ts=1700000005000000000\n"
        "ADD BUY  LIMIT 104.00  90 6 ts=1700000006000000000\n"
        "MODIFY 6 60              ts=1700000007000000000\n"
        "ADD SELL LIMIT 104.00  25 7 ts=1700000008000000000\n"   // lifts part of the bid
        "CANCEL 7                 ts=1700000009000000000\n"
        "ADD SELL LIMIT 105.50  80 8 ts=1700000010000000000\n"
        "ADD BUY  LIMIT 105.00  30 9 ts=1700000011000000000\n"   // re-quotes the top
        "ADD BUY  MARKET  20 10       ts=1700000012000000000\n"
        "ADD SELL LIMIT 104.00  10 11 ts=1700000013000000000\n";

    OrderBook book("AAPL", 0.01);
    Pipeline pipe(book);
    MicrostructureAnalyzer ana;
    pipe.set_analyzer(&ana);

    pipe.submit_text(feed);

    std::string err;
    std::cout << "invariants: " << (book.validate(&err) ? "OK" : "VIOLATED -> " + err) << "\n\n";
    std::cout << ana.report();

    // The per-observation series is the actual product: one row per book
    // change, ready to be stored or charted.
    std::cout << "--- observation series (quote, imbalance, flow) ---\n";
    std::cout << "   step     bid     ask   spread  depth_imb  liq_imb       OFI    VWAP\n";
    const auto& h = ana.history();
    for (size_t i = 0; i < h.size(); ++i) {
        const auto& m = h[i];
        std::cout << std::setw(7) << i
                  << cell(m.best_bid) << cell(m.best_ask) << cell(m.spread)
                  << cell(m.depth_imbalance) << cell(m.liquidity_imbalance)
                  << cell(m.ofi) << cell(m.vwap) << "\n";
    }
    std::cout << std::defaultfloat;
}

// Two populations in one session, because the only honest way to show a
// detector is to show what it declines to flag.
//
//   phase 1  a market maker working normally: small orders posted and pulled in
//            100ms, over and over. Every one of them is a rapid cancellation of
//            an order that stayed up briefly. A naive rule flags all of it.
//   phase 2  the same shape, scaled up and repeated: large orders 13 ticks above
//            the touch, the mid falling while each one is up, then pulled.
static std::string px_str(int cents) {
    return std::to_string(cents / 100) + "." +
           (cents % 100 < 10 ? "0" : "") + std::to_string(cents % 100);
}

static void demo_anomaly() {
    std::cout << "\n\n=== 15. Anomaly / Spoofing Detection ===\n";
    std::cout << "   rule-based, over the order stream the pipeline already collects\n\n";

    const uint64_t t0   = 1700000000000000000ULL;
    const uint64_t step = 100000000ULL;   // 100ms
    int tick = 0;
    OrderId id = 1;

    auto line = [&](std::string& buf, const std::string& body) {
        buf += "ts=" + std::to_string(t0 + static_cast<uint64_t>(tick++) * step) +
               " " + body + "\n";
    };

    // --- one book, and a market maker working it -------------------------
    std::string phase1;
    // Six resting bid levels of 400 lots each, and one offer at 100.10. The
    // spoof rounds drop the top bid a level at a time, so the mid grinds lower
    // while each large sell is up.
    for (int l = 0; l < 6; ++l) {
        for (int k = 0; k < 20; ++k) {
            line(phase1, "ADD BUY LIMIT " + px_str(9990 - 10 * l) + " 20 " +
                         std::to_string(id++));
        }
    }
    for (int k = 0; k < 20; ++k) {
        line(phase1, "ADD SELL LIMIT 100.10 20 " + std::to_string(id++));
    }
    // 60 small orders, each pulled 100ms later, none large against a 20-lot
    // baseline.
    for (int i = 0; i < 60; ++i) {
        const OrderId b = id++;
        line(phase1, "ADD BUY LIMIT 98.00 20 " + std::to_string(b));
        line(phase1, "CANCEL " + std::to_string(b));
    }

    // --- the pattern -----------------------------------------------------
    std::string phase2;
    for (int r = 0; r < 6; ++r) {
        const OrderId spoof = id++;
        // 900 lots, 13 ticks above the touch: it cannot trade, so pulling it
        // later is a decision rather than a consequence.
        line(phase2, "ADD SELL LIMIT 100.30 900 " + std::to_string(spoof));
        // A genuine sell lifting the whole top bid, so the mid falls 5 ticks
        // while the large sell is still resting.
        line(phase2, "ADD SELL LIMIT " + px_str(9990 - 10 * r) + " 400 " +
                     std::to_string(id++));
        line(phase2, "CANCEL " + std::to_string(spoof));
    }

    OrderBook book("AAPL", 0.01);
    Pipeline pipe(book);
    SpoofDetector det;
    pipe.set_detector(&det);

    pipe.submit_text(phase1);
    std::cout << "--- phase 1: 60 small orders, each pulled after 100ms ---\n";
    std::cout << "orders tracked : " << det.orders_seen()
              << ", large: " << det.orders_large() << "\n";
    std::cout << "lifetime       : mean " << static_cast<int>(det.mean_order_lifetime_us() / 1000)
              << "ms   (every one of them was pulled in 100ms)\n";
    std::cout << "cancel / place : " << std::fixed << std::setprecision(1)
              << det.cancel_to_placement_ratio() * 100.0
              << "%   of all placements, 120 of which were left to rest\n";
    std::cout << "alerts         : " << det.alerts_raised()
              << "   <- 60 orders placed and pulled in 100ms, none of them a pattern\n\n";
    std::cout << std::defaultfloat;

    pipe.submit_text(phase2);

    std::string err;
    std::cout << "--- phase 2: 6 large sells, 13 ticks up, pulled 200ms later ---\n";
    std::cout << "invariants: " << (book.validate(&err) ? "OK" : "VIOLATED -> " + err)
              << "\n\n";
    std::cout << det.report();
}

static void show(const char* label, const ExecutionReport& r) {
    std::cout << label << ": status=" << r.status_str()
              << " filled=" << r.filled_qty << "/" << r.requested_qty
              << " leaves=" << r.leaves_qty
              << " matches=" << r.match_count();
    if (r.avg_price()) {
        std::cout << " avg_px=" << std::fixed << std::setprecision(2) << *r.avg_price();
    }
    if (!r.accepted()) {
        std::cout << " reason=\"" << r.reject_reason() << "\"";
    }
    std::cout << "\n";
    for (const auto& f : r.fills) {
        std::cout << "    fill " << f.qty << " @ " << std::fixed << std::setprecision(2)
                  << f.price << " against resting order " << f.resting_id << "\n";
    }
}

int main() {
    OrderBook book("BTC/USD");

    std::cout << "=== 1. Populate Order Book ===\n";
    book.add_limit_order(OrderSide::BUY,  100.00, 10);
    book.add_limit_order(OrderSide::BUY,  99.50,  20);
    book.add_limit_order(OrderSide::BUY,  99.00,  15);
    book.add_limit_order(OrderSide::SELL, 100.50, 12);
    book.add_limit_order(OrderSide::SELL, 101.00, 8);
    book.add_limit_order(OrderSide::SELL, 101.50, 25);

    std::cout << "\n--- Initial Book ---\n";
    book.print_book();

    std::cout << "\n=== 2. Aggressive Buy crosses two ask levels ===\n";
    show("BUY 15 @ 101.00", book.add_limit_order(OrderSide::BUY, 101.00, 15));

    std::cout << "\n--- After crossing buy ---\n";
    book.print_book();

    std::cout << "\n=== 3. Partial Fill (buy 50 @ 101.00 but only 8 resting) ===\n";
    ExecutionReport partial = book.add_limit_order(OrderSide::BUY, 101.00, 50);
    show("BUY 50 @ 101.00", partial);

    std::cout << "\n--- After partial fill ---\n";
    book.print_book();

    const Order* op = book.get_order(partial.order_id);
    if (op) std::cout << "Order " << partial.order_id << " status: " << op->status_str()
                      << " filled=" << op->filled_qty << "/" << op->qty << "\n";

    std::cout << "\n=== 4. Cancel Order (order 3, resting BUY 99.00) ===\n";
    bool cancelled = book.cancel_order(3);
    std::cout << "Cancel returned: " << (cancelled ? "true" : "false") << "\n";
    const Order* c3 = book.get_order(3);
    if (c3) std::cout << "Order 3 status: " << c3->status_str() << "\n";

    std::cout << "\n--- After cancel ---\n";
    book.print_book();

    std::cout << "\n=== 5. Modify Order (order 2, BUY 99.50, increase 20 -> 35, loses priority) ===\n";
    bool modified = book.modify_order(2, 35);
    std::cout << "Modify returned: " << (modified ? "true" : "false") << "\n";
    const Order* m2 = book.get_order(2);
    if (m2) std::cout << "Order 2 status: " << m2->status_str()
                      << " qty=" << m2->qty << " filled=" << m2->filled_qty << "\n";

    std::cout << "\n--- After modify ---\n";
    book.print_book();

    std::cout << "\n=== 6. Market Order (buy 30) ===\n";
    show("BUY 30 MARKET", book.add_market_order(OrderSide::BUY, 30));

    std::cout << "\n--- After market buy ---\n";
    book.print_book();

    std::cout << "\n=== 7. Order validation: every bad submission is refused ===\n";
    show("BUY 0 @ 100.00 (zero qty)",   book.add_limit_order(OrderSide::BUY, 100.00, 0));
    show("BUY 10 @ -5.00 (negative)",   book.add_limit_order(OrderSide::BUY, -5.00, 10));
    show("BUY 10 @ NaN (not finite)",   book.add_limit_order(OrderSide::BUY,
                                    std::numeric_limits<double>::quiet_NaN(), 10));

    std::cout << "\n=== 8. Order lifecycle: NEW -> OPEN -> PARTIALLY_FILLED -> FILLED ===\n";
    OrderBook life("LIFE");
    auto lc = life.add_limit_order(OrderSide::BUY, 50.0, 10);
    std::cout << "after submit:        " << life.get_order(lc.order_id)->status_str() << "\n";
    life.add_limit_order(OrderSide::SELL, 50.0, 4);
    std::cout << "after 4 filled:      " << life.get_order(lc.order_id)->status_str() << "\n";
    life.add_limit_order(OrderSide::SELL, 50.0, 6);
    std::cout << "after 6 more:        " << life.get_order(lc.order_id)->status_str() << "\n";

    auto killed = life.add_limit_order(OrderSide::BUY, 40.0, 5);
    life.cancel_order(killed.order_id);
    std::cout << "after cancel:        " << life.get_order(killed.order_id)->status_str() << "\n";

    std::cout << "\nRecorded life of order " << lc.order_id << ":\n";
    for (const auto& t : life.order_history(lc.order_id)) {
        std::cout << "    seq=" << t.seq << "  " << status_name(t.from)
                  << " -> " << status_name(t.to) << "\n";
    }

    std::cout << "\n=== 9. Client-supplied order ids (idempotent retries) ===\n";
    show("SELL 7 @ 60.00, id=1001", life.add_limit_order(OrderSide::SELL, 60.0, 7, 1001));
    show("SELL 7 @ 60.00, id=1001 again", life.add_limit_order(OrderSide::SELL, 60.0, 7, 1001));

    std::cout << "\n=== 10. L2 Snapshot (depth=3) ===\n";
    auto snap = book.depth(3);
    snap.print();

    std::cout << "\n=== 11. Trade Journal ===\n";
    std::cout << "Total trades: " << book.trades().size() << "  volume=" << book.total_volume() << "  notional=" << std::fixed << std::setprecision(2) << book.total_notional() << "\n" << std::defaultfloat << "\n";
    for (const auto& t : book.trades()) {
        std::cout << "  trade " << t.trade_id << " seq=" << t.seq
                  << " aggressor=" << t.aggressor_id
                  << " passive=" << t.passive_id
                  << (t.side == OrderSide::BUY ? " BUY " : " SELL ")
                  << " px=" << std::fixed << std::setprecision(2) << t.price
                  << " qty=" << t.qty << "\n";
    }

    std::cout << "\n=== 12. Event Stream (audit trail) ===\n";
    book.event_stream().print();

    // --- Feature 4: the event / market data pipeline ----------------------
    // A real session arrives as a feed, not as hand-placed orders. This drives
    // a second book entirely from text and reads the analytics back out.
    demo_pipeline();

    // --- Feature 5: market microstructure -------------------------------
    // The same feed, measured: what the book and the flow say about it.
    demo_microstructure();

    // --- Feature 6: anomaly / spoofing detection -------------------------
    // Rule-based, and judged against a benign population it must not flag.
    demo_anomaly();

    std::cout << "\nTotal orders in book: " << book.total_orders() << "\n";
    std::cout << "Bid levels: " << book.bid_levels() << " Ask levels: " << book.ask_levels() << "\n";
    std::cout << "Sequence: " << book.sequence() << "\n";

    std::string err;
    std::cout << "Invariants: " << (book.validate(&err) ? "OK" : "VIOLATED -> " + err) << "\n";

    return 0;
}
