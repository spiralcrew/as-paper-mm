/**
 * @file binance_parser.cpp
 * @brief Минимальный парсер JSON-сообщений Binance.
 */
#include "asmm/binance_parser.hpp"

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <new>
#include <system_error>
#include <version>

namespace asmm {

namespace {

bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/**
 * Найти поле `"key": value` и вернуть позицию начала значения.
 * Ключ должен быть окружён кавычками и сразу за ним идти двоеточие, поэтому
 * `"b"` не совпадёт с `"B"` или с `"bookTicker"`.
 */
std::optional<std::size_t> find_value(std::string_view json, std::string_view key) noexcept {
    std::size_t pos = 0;
    while ((pos = json.find(key, pos)) != std::string_view::npos) {
        const std::size_t end = pos + key.size();
        if (pos > 0 && json[pos - 1] == '"' && end + 1 < json.size() && json[end] == '"' &&
            json[end + 1] == ':') {
            std::size_t v = end + 2;
            while (v < json.size() && is_space(json[v])) {
                ++v;
            }
            if (v >= json.size()) {
                return std::nullopt;
            }
            return v;
        }
        pos = end;
    }
    return std::nullopt;
}

/// Значение плоского поля (строка без кавычек, число или литерал).
std::optional<std::string_view> find_field(std::string_view json, std::string_view key) noexcept {
    const auto start = find_value(json, key);
    if (!start) {
        return std::nullopt;
    }
    const std::size_t v = *start;
    if (json[v] == '"') {
        const std::size_t close = json.find('"', v + 1);
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        return json.substr(v + 1, close - v - 1);
    }
    if (json[v] == '[' || json[v] == '{') {
        return std::nullopt;
    }
    std::size_t e = v;
    while (e < json.size() && json[e] != ',' && json[e] != '}' && json[e] != ']' &&
           !is_space(json[e])) {
        ++e;
    }
    return json.substr(v, e - v);
}

bool to_double(std::string_view s, double& out) noexcept {
    if (s.empty()) {
        return false;
    }
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size();
#else
    char buf[64];
    if (s.size() >= sizeof(buf)) {
        return false;
    }
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    char* end = nullptr;
    out = std::strtod(buf, &end);
    return end == buf + s.size();
#endif
}

template <typename Int>
bool to_int(std::string_view s, Int& out) noexcept {
    if (s.empty()) {
        return false;
    }
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

bool get_double(std::string_view json, std::string_view key, double& out) noexcept {
    const auto v = find_field(json, key);
    return v && to_double(*v, out);
}

template <typename Int>
bool get_int(std::string_view json, std::string_view key, Int& out) noexcept {
    const auto v = find_field(json, key);
    return v && to_int(*v, out);
}

bool ends_with(std::string_view s, std::string_view suffix) noexcept {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

void skip_spaces(std::string_view json, std::size_t& i) noexcept {
    while (i < json.size() && is_space(json[i])) {
        ++i;
    }
}

bool read_string(std::string_view json, std::size_t& i, std::string_view& out) noexcept {
    skip_spaces(json, i);
    if (i >= json.size() || json[i] != '"') {
        return false;
    }
    const std::size_t close = json.find('"', i + 1);
    if (close == std::string_view::npos) {
        return false;
    }
    out = json.substr(i + 1, close - i - 1);
    i = close + 1;
    return true;
}

bool expect(std::string_view json, std::size_t& i, char c) noexcept {
    skip_spaces(json, i);
    if (i >= json.size() || json[i] != c) {
        return false;
    }
    ++i;
    return true;
}

/// Разобрать массив уровней `[["price","qty"], ...]` из поля `key`.
bool parse_levels(std::string_view json, std::string_view key, std::vector<PriceLevel>& out) {
    const auto start = find_value(json, key);
    if (!start || json[*start] != '[') {
        return false;
    }
    std::size_t i = *start + 1;
    while (true) {
        skip_spaces(json, i);
        if (i >= json.size()) {
            return false;
        }
        if (json[i] == ']') {
            return true;
        }
        if (json[i] == ',') {
            ++i;
            continue;
        }
        std::string_view price_text;
        std::string_view qty_text;
        PriceLevel level;
        if (!expect(json, i, '[') || !read_string(json, i, price_text) || !expect(json, i, ',') ||
            !read_string(json, i, qty_text) || !expect(json, i, ']') ||
            !to_double(price_text, level.price) || !to_double(qty_text, level.qty)) {
            return false;
        }
        out.push_back(level);
    }
}

std::optional<ParsedMessage> parse_book(std::string_view data, std::int64_t recv_ms) noexcept {
    ParsedMessage msg;
    msg.type = MessageType::BookTicker;
    BookTop& b = msg.book;
    if (!get_double(data, "b", b.bid_price) || !get_double(data, "B", b.bid_qty) ||
        !get_double(data, "a", b.ask_price) || !get_double(data, "A", b.ask_qty)) {
        return std::nullopt;
    }
    get_int(data, "u", b.update_id);
    b.ts_ms = recv_ms;
    return msg;
}

std::optional<ParsedMessage> parse_trade(std::string_view data, std::int64_t recv_ms,
                                         bool aggregated) noexcept {
    ParsedMessage msg;
    msg.type = MessageType::Trade;
    asmm::Trade& t = msg.trade;
    if (!get_double(data, "p", t.price) || !get_double(data, "q", t.qty)) {
        return std::nullopt;
    }
    const auto maker = find_field(data, "m");
    if (!maker || (*maker != "true" && *maker != "false")) {
        return std::nullopt;
    }
    t.buyer_is_maker = *maker == "true";
    get_int(data, aggregated ? "a" : "t", t.trade_id);
    std::int64_t trade_time = 0;
    get_int(data, "T", trade_time);
    t.ts_ms = recv_ms > 0 ? recv_ms : trade_time;
    return msg;
}

std::optional<ParsedMessage> parse_depth_update(std::string_view data, std::int64_t recv_ms) {
    ParsedMessage msg;
    msg.type = MessageType::DepthUpdate;
    asmm::DepthUpdate& d = msg.depth;
    if (!get_int(data, "U", d.first_update_id) || !get_int(data, "u", d.final_update_id) ||
        d.first_update_id > d.final_update_id || !parse_levels(data, "b", d.bids) ||
        !parse_levels(data, "a", d.asks)) {
        return std::nullopt;
    }
    std::int64_t event_time = 0;
    get_int(data, "E", event_time);
    d.ts_ms = recv_ms > 0 ? recv_ms : event_time;
    return msg;
}

std::optional<ParsedMessage> parse_depth_snapshot(std::string_view data, std::int64_t recv_ms) {
    ParsedMessage msg;
    msg.type = MessageType::DepthSnapshot;
    asmm::DepthSnapshot& s = msg.snapshot;
    if (!get_int(data, "lastUpdateId", s.last_update_id) || !parse_levels(data, "bids", s.bids) ||
        !parse_levels(data, "asks", s.asks)) {
        return std::nullopt;
    }
    s.ts_ms = recv_ms;
    return msg;
}

/// `<symbol>@depth` или `<symbol>@depth@100ms|1000ms` (diff depth, не partial depth).
bool is_diff_depth_stream(std::string_view stream) noexcept {
    const std::size_t pos = stream.find("@depth");
    if (pos == std::string_view::npos) {
        return false;
    }
    const std::string_view rest = stream.substr(pos + 6);
    return rest.empty() || rest == "@100ms" || rest == "@1000ms";
}

std::optional<ParsedMessage> parse_impl(std::string_view line) {
    std::int64_t recv_ms = 0;
    get_int(line, "recv_ms", recv_ms);

    std::string_view data = line;
    std::string_view stream;
    if (const auto s = find_field(line, "stream")) {
        stream = *s;
        const auto data_pos = find_value(line, "data");
        if (!data_pos) {
            return std::nullopt;
        }
        data = line.substr(*data_pos);
    }

    if (!stream.empty()) {
        if (ends_with(stream, "@bookTicker")) {
            return parse_book(data, recv_ms);
        }
        if (ends_with(stream, "@trade")) {
            return parse_trade(data, recv_ms, false);
        }
        if (ends_with(stream, "@aggTrade")) {
            return parse_trade(data, recv_ms, true);
        }
        if (ends_with(stream, "@depthSnapshot")) {
            return parse_depth_snapshot(data, recv_ms);
        }
        if (is_diff_depth_stream(stream)) {
            return parse_depth_update(data, recv_ms);
        }
        return std::nullopt;
    }

    // Сырое сообщение одиночного стрима: у событий есть поле "e", у bookTicker и снимка — нет.
    if (const auto event = find_field(data, "e")) {
        if (*event == "trade") {
            return parse_trade(data, recv_ms, false);
        }
        if (*event == "aggTrade") {
            return parse_trade(data, recv_ms, true);
        }
        if (*event == "depthUpdate") {
            return parse_depth_update(data, recv_ms);
        }
        return std::nullopt;
    }
    if (find_value(data, "lastUpdateId")) {
        return parse_depth_snapshot(data, recv_ms);
    }
    return parse_book(data, recv_ms);
}

}  // namespace

std::optional<ParsedMessage> parse_binance_message(std::string_view line) noexcept {
    try {
        return parse_impl(line);
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
}

}  // namespace asmm
