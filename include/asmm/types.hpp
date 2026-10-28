/**
 * @file types.hpp
 * @brief Базовые рыночные типы: лучшие цены стакана, сделка, сторона заявки.
 */
#pragma once

#include <cstdint>

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

}  // namespace asmm
