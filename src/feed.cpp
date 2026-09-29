#include "feed.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace {

std::vector<std::string> tokenize(std::string_view line) {
    std::vector<std::string> out;
    std::istringstream ss{std::string(line)};
    std::string tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}

bool parse_u64(const std::string& s, uint64_t& out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') return false;
    out = static_cast<uint64_t>(v);
    return true;
}

// Accepts what strtod accepts, including inf/nan spellings, so the feed parser
// stays a parser: judging a price is the engine's job, and a NaN that gets
// this far is refused by validation rather than silently dropped here.
bool parse_double(const std::string& s, double& out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') return false;
    out = v;
    return true;
}

FeedParseResult fail(size_t line_no, std::string_view line, const std::string& msg) {
    FeedParseResult r;
    r.ok = false;
    r.error = msg;
    r.line_no = line_no;
    r.event.raw = std::string(line);
    return r;
}

}  // namespace

FeedParseResult FeedParser::parse(std::string_view line, size_t line_no) {
    // Trim, so an indented comment is still a comment.
    size_t b = 0;
    while (b < line.size() && (line[b] == ' ' || line[b] == '\t' || line[b] == '\r')) ++b;
    size_t e = line.size();
    while (e > b && (line[e - 1] == ' ' || line[e - 1] == '\t' || line[e - 1] == '\r')) --e;
    std::string_view trimmed = line.substr(b, e - b);

    // A trailing comment runs to end of line. No field can contain '#' -- verbs
    // are alphabetic and every other field is numeric -- so this is unambiguous.
    if (size_t hash = trimmed.find('#'); hash != std::string_view::npos) {
        trimmed = trimmed.substr(0, hash);
        while (!trimmed.empty() &&
               (trimmed.back() == ' ' || trimmed.back() == '\t')) {
            trimmed.remove_suffix(1);
        }
    }

    FeedParseResult r;
    r.line_no = line_no;
    r.event.raw = std::string(line);

    if (trimmed.empty() || trimmed[0] == '#') {
        r.ok = true;
        r.event.type = FeedEventType::NONE;
        return r;
    }

    std::vector<std::string> tok = tokenize(trimmed);
    if (tok.empty()) {
        r.ok = true;
        r.event.type = FeedEventType::NONE;
        return r;
    }

    // An optional venue timestamp may appear anywhere; pull it out first so it
    // cannot be mistaken for a positional field.
    std::vector<std::string> args;
    args.reserve(tok.size());
    for (auto& t : tok) {
        if (t.rfind("ts=", 0) == 0) {
            uint64_t ts = 0;
            if (!parse_u64(t.substr(3), ts)) {
                return fail(line_no, line, "malformed timestamp in '" + t + "'");
            }
            r.event.exchange_ts = ts;
        } else {
            args.push_back(std::move(t));
        }
    }

    if (args.empty()) return fail(line_no, line, "missing verb");

    // Taken from the de-timestamped tokens, not from tok[0]. Reading it from
    // tok[0] meant any line beginning with `ts=` reported the *timestamp* as
    // the unknown verb, which is precisely the wrong token to name when you are
    // trying to work out why a feed is being ignored.
    r.event.verb = args[0];
    const std::string& verb = args[0];
    auto need = [&](size_t n) { return args.size() == n + 1; };

    if (verb == "ADD") {
        if (args.size() < 3) return fail(line_no, line, "ADD wants: <side> <type> ...");
        r.event.type = FeedEventType::ADD;

        if (args[1] == "BUY")       r.event.side = OrderSide::BUY;
        else if (args[1] == "SELL") r.event.side = OrderSide::SELL;
        else return fail(line_no, line, "side must be BUY or SELL, got '" + args[1] + "'");

        if (args[2] == "LIMIT")       r.event.order_type = OrderType::LIMIT;
        else if (args[2] == "MARKET") r.event.order_type = OrderType::MARKET;
        else return fail(line_no, line, "type must be LIMIT or MARKET, got '" + args[2] + "'");

        // A market order has no price to send, so it is not asked for. The
        // engine substitutes a placeholder and never keys a level on it.
        if (r.event.order_type == OrderType::LIMIT) {
            if (args.size() != 6)
                return fail(line_no, line, "ADD LIMIT wants: <side> LIMIT <price> <qty> <id>");
            if (!parse_double(args[3], r.event.price))
                return fail(line_no, line, "price '" + args[3] + "' is not a number");
            if (!parse_u64(args[4], r.event.qty))
                return fail(line_no, line, "qty '" + args[4] + "' is not a whole number");
            if (!parse_u64(args[5], r.event.order_id))
                return fail(line_no, line, "order id '" + args[5] + "' is not a whole number");
        } else {
            if (args.size() != 5)
                return fail(line_no, line, "ADD MARKET wants: <side> MARKET <qty> <id>");
            if (!parse_u64(args[3], r.event.qty))
                return fail(line_no, line, "qty '" + args[3] + "' is not a whole number");
            if (!parse_u64(args[4], r.event.order_id))
                return fail(line_no, line, "order id '" + args[4] + "' is not a whole number");
        }

        r.ok = true;
        return r;
    }

    if (verb == "CANCEL") {
        if (!need(1)) return fail(line_no, line, "CANCEL wants: <id>");
        if (!parse_u64(args[1], r.event.order_id))
            return fail(line_no, line, "order id '" + args[1] + "' is not a whole number");
        r.event.type = FeedEventType::CANCEL;
        r.ok = true;
        return r;
    }

    if (verb == "MODIFY") {
        if (args.size() != 3 && args.size() != 4)
            return fail(line_no, line, "MODIFY wants: <id> <new_qty> [new_price]");
        r.event.type = FeedEventType::MODIFY;
        if (!parse_u64(args[1], r.event.order_id))
            return fail(line_no, line, "order id '" + args[1] + "' is not a whole number");
        if (!parse_u64(args[2], r.event.new_qty))
            return fail(line_no, line, "new qty '" + args[2] + "' is not a whole number");
        if (args.size() == 4) {
            if (!parse_double(args[3], r.event.new_price))
                return fail(line_no, line, "new price '" + args[3] + "' is not a number");
            r.event.has_new_price = true;
        }
        r.ok = true;
        return r;
    }

    if (verb == "TRADE") {
        if (args.size() != 5)
            return fail(line_no, line, "TRADE wants: <price> <qty> <buy_id> <sell_id>");
        r.event.type = FeedEventType::TRADE_PRINT;
        if (!parse_double(args[1], r.event.price))
            return fail(line_no, line, "price '" + args[1] + "' is not a number");
        if (!parse_u64(args[2], r.event.qty))
            return fail(line_no, line, "qty '" + args[2] + "' is not a whole number");
        if (!parse_u64(args[3], r.event.buy_order_id))
            return fail(line_no, line, "buy id '" + args[3] + "' is not a whole number");
        if (!parse_u64(args[4], r.event.sell_order_id))
            return fail(line_no, line, "sell id '" + args[4] + "' is not a whole number");
        r.ok = true;
        return r;
    }

    // Unrecognised verb: a well-formed line the book has no use for.
    r.ok = true;
    r.event.type = FeedEventType::UNKNOWN;
    return r;
}

std::vector<FeedParseResult> FeedParser::parse_all(std::string_view text) {
    std::vector<FeedParseResult> out;
    size_t line_no = 0;
    size_t start = 0;
    // A trailing newline ends the last line rather than starting an empty one,
    // matching std::getline.
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        std::string_view line = (nl == std::string_view::npos)
                                    ? text.substr(start)
                                    : text.substr(start, nl - start);
        ++line_no;
        FeedParseResult r = parse(line, line_no);
        if (r.event.type != FeedEventType::NONE) out.push_back(std::move(r));
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return out;
}
