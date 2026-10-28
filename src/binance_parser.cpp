/**
 * @file binance_parser.cpp
 * @brief Минимальный парсер JSON-сообщений Binance без аллокаций.
 */
#include "asmm/binance_parser.hpp"

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <version>

namespace asmm {

namespace {

bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/**
 * Найти значение плоского поля `"key": value` и вернуть его текст без кавычек.
 * Ключ должен быть окружён кавычками и сразу за ним идти двоеточие, поэтому
 * `"b"` не совпадёт с `"B"` или с `"bookTicker"`.
 */
std::optional<std::string_view> find_field(std::string_view json, std::string_view key) noexcept {
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
            if (json[v] == '"') {
                const std::size_t close = json.find('"', v + 1);
                if (close == std::string_view::npos) {
                    return std::nullopt;
                }
                return json.substr(v + 1, close - v - 1);
            }
            std::size_t e = v;
            while (e < json.size() && json[e] != ',' && json[e] != '}' && !is_space(json[e])) {
                ++e;
            }
            return json.substr(v, e - v);
        }
        pos = end;
    }
    return std::nullopt;
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

}  // namespace

std::optional<ParsedMessage> parse_binance_message(std::string_view line) noexcept {
    std::int64_t recv_ms = 0;
    get_int(line, "recv_ms", recv_ms);

    std::string_view data = line;
    std::string_view stream;
    if (const auto s = find_field(line, "stream")) {
        stream = *s;
        const std::size_t data_pos = line.find("\"data\":");
        if (data_pos == std::string_view::npos) {
            return std::nullopt;
        }
        data = line.substr(data_pos + 7);
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
        return std::nullopt;
    }

    // Сырое сообщение одиночного стрима: у сделок есть поле "e", у bookTicker — нет.
    if (const auto event = find_field(data, "e")) {
        if (*event == "trade") {
            return parse_trade(data, recv_ms, false);
        }
        if (*event == "aggTrade") {
            return parse_trade(data, recv_ms, true);
        }
        return std::nullopt;
    }
    return parse_book(data, recv_ms);
}

}  // namespace asmm
