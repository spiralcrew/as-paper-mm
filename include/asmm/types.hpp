/**
 * @file types.hpp
 * @brief Базовые рыночные типы: сторона заявки, лучшие цены, сделка, уровни и обновления стакана.
 */
#pragma once

#include <cstdint>
#include <vector>

namespace asmm {

/// Сторона заявки.
enum class Side : std::uint8_t {
    Buy,  ///< покупка (наш бид)
    Sell  ///< продажа (наш аск)
};

/**
 * @brief Лучшие цены стакана (top of book), сообщение Binance `bookTicker`.
 *
 * Цены — в USDT, объёмы — в BTC.
 */
struct BookTop {
    double bid_price = 0.0;       ///< лучшая цена покупки
    double bid_qty = 0.0;         ///< объём на лучшем биде
    double ask_price = 0.0;       ///< лучшая цена продажи
    double ask_qty = 0.0;         ///< объём на лучшем аске
    std::uint64_t update_id = 0;  ///< `u` — id обновления стакана Binance
    std::int64_t ts_ms = 0;       ///< локальное время получения, мс Unix; 0 — неизвестно

    /// Середина спреда.
    [[nodiscard]] double mid() const noexcept { return 0.5 * (bid_price + ask_price); }

    /// Ширина спреда.
    [[nodiscard]] double spread() const noexcept { return ask_price - bid_price; }

    /// Стакан непустой и не перекрещен.
    [[nodiscard]] bool valid() const noexcept { return bid_price > 0.0 && ask_price > bid_price; }
};

/// Публичная сделка на бирже, сообщение Binance `trade` / `aggTrade`.
struct Trade {
    double price = 0.0;           ///< цена сделки, USDT
    double qty = 0.0;             ///< объём сделки, BTC
    bool buyer_is_maker = false;  ///< `m`: true — агрессор продавец (бьёт в биды)
    std::uint64_t trade_id = 0;   ///< id сделки
    std::int64_t ts_ms = 0;       ///< время, мс Unix (локальное получение или `T`)
};

/// Уровень стакана: цена и суммарный объём заявок на ней.
struct PriceLevel {
    double price = 0.0;  ///< цена, USDT
    double qty = 0.0;    ///< объём, BTC; 0 в обновлении — уровень удалён
};

/**
 * @brief Снимок стакана (REST `GET /api/v3/depth`), точка синхронизации локальной книги.
 *
 * Уровни — от лучшего к худшему.
 */
struct DepthSnapshot {
    std::uint64_t last_update_id = 0;  ///< `lastUpdateId`
    std::vector<PriceLevel> bids;      ///< биды
    std::vector<PriceLevel> asks;      ///< аски
    std::int64_t ts_ms = 0;            ///< локальное время получения, мс
};

/**
 * @brief Инкрементальное обновление стакана (WebSocket `<symbol>@depth@100ms`).
 *
 * Содержит абсолютные новые объёмы всех изменившихся уровней за интервал
 * обновлений `[first_update_id, final_update_id]`.
 */
struct DepthUpdate {
    std::uint64_t first_update_id = 0;  ///< `U`
    std::uint64_t final_update_id = 0;  ///< `u`
    std::vector<PriceLevel> bids;       ///< изменившиеся биды
    std::vector<PriceLevel> asks;       ///< изменившиеся аски
    std::int64_t ts_ms = 0;             ///< локальное время получения или `E`, мс
};

}  // namespace asmm
