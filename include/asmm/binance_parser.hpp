/**
 * @file binance_parser.hpp
 * @brief Разбор сообщений публичных WebSocket-стримов Binance Spot.
 *
 * Поддерживаемые стримы
 * (https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams):
 * - `<symbol>@bookTicker` — лучшие бид/аск;
 * - `<symbol>@trade`, `<symbol>@aggTrade` — сделки.
 *
 * Принимаются как «сырые» сообщения одиночного стрима, так и обёртка combined stream
 * `{"stream":"...","data":{...}}`. Опциональное поле верхнего уровня `recv_ms`
 * (локальное время получения, добавляет `scripts/binance_feed.py`) становится временем события.
 */
#pragma once

#include <optional>
#include <string_view>

#include "asmm/types.hpp"

namespace asmm {

/// Тип распознанного сообщения.
enum class MessageType : std::uint8_t {
    BookTicker,  ///< лучшие цены стакана
    Trade        ///< сделка
};

/// Результат разбора: заполнено поле, соответствующее `type`.
struct ParsedMessage {
    MessageType type = MessageType::BookTicker;  ///< тип сообщения
    BookTop book;                                ///< для `BookTicker`
    asmm::Trade trade;                           ///< для `Trade`
};

/**
 * @brief Разобрать одну строку JSON от Binance.
 *
 * Парсер намеренно минимальный и без аллокаций: извлекает только известные плоские
 * поля. Ошибки и неизвестные сообщения возвращают `std::nullopt`.
 */
[[nodiscard]] std::optional<ParsedMessage> parse_binance_message(std::string_view line) noexcept;

}  // namespace asmm
