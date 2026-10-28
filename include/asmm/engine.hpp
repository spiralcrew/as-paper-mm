/**
 * @file engine.hpp
 * @brief Движок paper market making: поток данных → волатильность → котировки AS → симуляция
 * исполнения.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

#include "asmm/avellaneda_stoikov.hpp"
#include "asmm/binance_parser.hpp"
#include "asmm/paper_exchange.hpp"
#include "asmm/volatility.hpp"

namespace asmm {

/// Конфигурация движка. Значения по умолчанию подобраны под BTCUSDT.
struct EngineConfig {
    AsParams as;                              ///< параметры модели
    double tick_size = 0.01;                  ///< шаг цены BTCUSDT, USDT
    double order_qty = 0.001;                 ///< размер одной заявки (1 лот), BTC
    double max_inventory_lots = 5.0;          ///< лимит |q|, после него сторона снимается
    double vol_half_life_sec = 60.0;          ///< полураспад EWMA-волатильности
    std::int64_t vol_min_interval_ms = 1000;  ///< шаг сэмплирования волатильности
    double initial_sigma = 2.0;               ///< σ до прогрева, USDT/√s
    FillModel fill_model = FillModel::Touch;  ///< правило симуляции исполнения
    double maker_fee_rate = 0.0;              ///< комиссия мейкера (доля notional)
};

/// Снимок состояния движка.
struct EngineStats {
    std::int64_t ts_ms = 0;          ///< текущее время данных, мс
    double mid = 0.0;                ///< mid-цена
    double sigma = 0.0;              ///< оценка σ, USDT/√s
    double tau = 0.0;                ///< время до конца сессии, с
    double reservation_price = 0.0;  ///< резервационная цена r
    double bid = 0.0;                ///< наша цена бид
    double ask = 0.0;                ///< наша цена аск
    bool bid_active = false;         ///< бид выставлен
    bool ask_active = false;         ///< аск выставлен
    double inventory_btc = 0.0;      ///< позиция, BTC
    double inventory_lots = 0.0;     ///< позиция в лотах q
    double cash = 0.0;               ///< кэш, USDT
    double equity = 0.0;             ///< PnL mark-to-market, USDT
    double fees = 0.0;               ///< комиссии, USDT
    double volume_quote = 0.0;       ///< оборот, USDT
    std::size_t fills = 0;           ///< число исполнений
    std::size_t book_updates = 0;    ///< обработано обновлений стакана
    std::size_t trades = 0;          ///< обработано сделок
};

/**
 * @brief Маркет-мейкер Avellaneda–Stoikov поверх виртуальной биржи.
 *
 * На каждом обновлении стакана: обновить σ, вычислить \f$ \tau \f$, посчитать котировки,
 * округлить к сетке (post-only) и переставить виртуальные заявки. На каждой сделке:
 * проверить исполнение и, если оно было, перекотироваться с новым инвентарём.
 *
 * Горизонт скользящий: сессии длиной `horizon_sec` идут подряд, \f$ \tau \f$ убывает от
 * \f$ T \f$ до 0 и сбрасывается. Инвентарь между сессиями не ликвидируется.
 */
class MarketMakerEngine {
public:
    /// Обработчик исполнений (например, для логирования).
    using FillCallback = std::function<void(const Fill&)>;

    /// @throws std::invalid_argument при недопустимой конфигурации
    explicit MarketMakerEngine(const EngineConfig& config);

    /// Обработать обновление лучших цен.
    void on_book(const BookTop& book);

    /// Обработать сделку; возвращает число исполнений наших заявок (0 или 1).
    std::size_t on_trade(const Trade& trade);

    /// Обработать разобранное сообщение Binance.
    void on_message(const ParsedMessage& message);

    /// Установить обработчик исполнений.
    void set_fill_callback(FillCallback callback) { on_fill_ = std::move(callback); }

    /// Текущее состояние.
    [[nodiscard]] EngineStats stats() const;

    /// Время последнего события, мс.
    [[nodiscard]] std::int64_t now_ms() const noexcept { return now_ms_; }

    /// \f$ \tau \f$ в момент `ts_ms` при скользящем горизонте.
    [[nodiscard]] double tau_at(std::int64_t ts_ms) const noexcept;

    /// Конфигурация.
    [[nodiscard]] const EngineConfig& config() const noexcept { return config_; }

private:
    void advance_clock(std::int64_t ts_ms) noexcept;
    void requote();

    EngineConfig config_;
    EwmaVolatility vol_;
    PaperExchange exchange_;
    FillCallback on_fill_;
    BookTop book_;
    bool has_book_ = false;
    Quotes quotes_;
    double tau_ = 0.0;
    std::int64_t now_ms_ = 0;
    std::int64_t session_start_ms_ = -1;
    std::size_t book_updates_ = 0;
    std::size_t trades_ = 0;
};

}  // namespace asmm
