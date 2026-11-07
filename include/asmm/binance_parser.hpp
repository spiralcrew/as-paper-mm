/**
 * @file binance_parser.hpp
 * @brief Разбор сообщений публичных стримов Binance Spot.
 *
 * Поддерживаемые сообщения
 * (https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams):
 * - `<symbol>@bookTicker` — лучшие бид/аск;
 * - `<symbol>@trade`, `<symbol>@aggTrade` — сделки;
 * - `<symbol>@depth`, `<symbol>@depth@100ms` — инкрементальные обновления стакана (`depthUpdate`);
 * - `<symbol>@depthSnapshot` — снимок стакана REST `GET /api/v3/depth`, который
 *   `scripts/binance_feed.py` вставляет в поток с этим именем стрима.
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
    BookTicker,     ///< лучшие цены стакана
    Trade,          ///< сделка
    DepthUpdate,    ///< инкрементальное обновление стакана
    DepthSnapshot,  ///< снимок стакана
};

/// Результат разбора: заполнено поле, соответствующее `type`.
struct ParsedMessage {
    MessageType type = MessageType::BookTicker;  ///< тип сообщения
    BookTop book;                                ///< для `BookTicker`
    asmm::Trade trade;                           ///< для `Trade`
    asmm::DepthUpdate depth;                     ///< для `DepthUpdate`
    asmm::DepthSnapshot snapshot;                ///< для `DepthSnapshot`
};

/**
 * @brief Разобрать одну строку JSON от Binance.
 *
 * Парсер минимальный и не строит DOM: извлекает только известные поля. Для `bookTicker` и
 * `trade` аллокаций нет; для стакана выделяются векторы уровней. Ошибки и неизвестные
 * сообщения возвращают `std::nullopt`.
 */
[[nodiscard]] std::optional<ParsedMessage> parse_binance_message(std::string_view line) noexcept;

}  // namespace asmm
