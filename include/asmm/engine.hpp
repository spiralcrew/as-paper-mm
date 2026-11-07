/**
 * @file engine.hpp
 * @brief Движок paper market making: поток данных → книга, σ, калибровка k →
 * котировки Avellaneda–Stoikov → симуляция исполнения.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "asmm/avellaneda_stoikov.hpp"
#include "asmm/binance_parser.hpp"
#include "asmm/intensity.hpp"
#include "asmm/order_book.hpp"
#include "asmm/paper_exchange.hpp"
#include "asmm/volatility.hpp"

namespace asmm {

/// Конфигурация движка. Значения по умолчанию подобраны под BTCUSDT.
struct EngineConfig {
    AsParams as;                              ///< γ, k (если калибровка выключена), T
    double tick_size = 0.01;                  ///< шаг цены BTCUSDT, USDT
    double order_qty = 0.001;                 ///< размер одной заявки (1 лот), BTC
    double max_inventory_lots = 5.0;          ///< лимит |q| в лотах
    double vol_half_life_sec = 60.0;          ///< полураспад EWMA-волатильности
    std::int64_t vol_min_interval_ms = 1000;  ///< шаг сэмплирования волатильности
    double initial_sigma = 2.0;               ///< σ до прогрева, USDT/√s
    bool calibrate_k = true;                  ///< оценивать k по ленте сделок
    double k_min = 0.1;                       ///< нижняя граница калиброванного k
    double k_max = 20.0;                      ///< верхняя граница калиброванного k
    IntensityConfig intensity;                ///< параметры калибровки λ(δ)
    ExchangeConfig exchange;                  ///< модель исполнения, задержка, комиссии
    int requote_ticks = 1;                    ///< минимальный сдвиг цены для перестановки, тики
    bool liquidate_at_session_end = false;    ///< закрывать позицию рыночной заявкой при τ → 0
};

/// Снимок состояния движка.
struct EngineStats {
    std::int64_t ts_ms = 0;               ///< текущее время данных, мс
    double mid = 0.0;                     ///< mid-цена
    double sigma = 0.0;                   ///< оценка σ, USDT/√s
    double tau = 0.0;                     ///< время до конца сессии, с
    double k = 0.0;                       ///< используемый k, 1/USDT
    double intensity_A = 0.0;             ///< откалиброванный A, 1/с (0 — нет оценки)
    bool k_calibrated = false;            ///< k взят из калибровки
    double k_fit = 0.0;                   ///< сырая оценка k до ограничения [k_min, k_max]
    double k_fit_r2 = 0.0;                ///< R² регрессии ln λ на δ
    double reservation_price = 0.0;       ///< резервационная цена r
    double spread = 0.0;                  ///< оптимальный спред модели
    double bid = 0.0;                     ///< наша цена бид
    double ask = 0.0;                     ///< наша цена аск
    bool bid_active = false;              ///< бид выставлен
    bool ask_active = false;              ///< аск выставлен
    double bid_queue = 0.0;               ///< объём перед нашим бидом, BTC
    double ask_queue = 0.0;               ///< объём перед нашим аском, BTC
    double inventory_btc = 0.0;           ///< позиция, BTC
    double inventory_lots = 0.0;          ///< позиция в лотах q
    double max_abs_inventory_lots = 0.0;  ///< максимум |q| за прогон
    double cash = 0.0;                    ///< кэш, USDT
    double equity = 0.0;                  ///< PnL mark-to-market, USDT
    double fees = 0.0;                    ///< комиссии, USDT
    double volume_quote = 0.0;            ///< мейкер-оборот, USDT
    double taker_volume = 0.0;            ///< тейкер-оборот, USDT
    double slippage_cost = 0.0;           ///< проскальзывание рыночных заявок, USDT
    double edge_captured = 0.0;           ///< Σ (mid − цена) по покупкам и (цена − mid) по продажам
    std::size_t fills = 0;                ///< мейкер-исполнений
    std::size_t taker_orders = 0;         ///< рыночных заявок
    std::size_t book_updates = 0;         ///< обновлений bookTicker
    std::size_t depth_updates = 0;        ///< применённых обновлений стакана
    std::size_t depth_snapshots = 0;      ///< снимков стакана
    std::size_t depth_gaps = 0;           ///< разрывов последовательности стакана
    bool book_synced = false;             ///< локальная книга синхронизирована
    std::size_t trades = 0;               ///< обработано сделок
    std::size_t session = 0;              ///< номер сессии длиной T
};

/**
 * @brief Маркет-мейкер Avellaneda–Stoikov поверх виртуальной биржи.
 *
 * На каждом обновлении цены: обновить σ и калибровку \f$ \lambda(\delta) \f$, вычислить
 * \f$ \tau \f$, посчитать котировки (резервационная цена + оптимальный спред с текущим k),
 * округлить к сетке (post-only), урезать объём под лимит инвентаря и переставить
 * виртуальные заявки (с гистерезисом `requote_ticks`, чтобы не терять очередь).
 * На каждой сделке: исполнение по модели очереди и перекотировка с новым инвентарём.
 *
 * Горизонт скользящий: сессии длиной `horizon_sec` идут подряд, \f$ \tau \f$ убывает от
 * \f$ T \f$ до 0 и сбрасывается. С `liquidate_at_session_end` позиция закрывается рыночной
 * заявкой в конце каждой сессии (терминальное условие статьи, но с реальным проскальзыванием).
 *
 * Объект не копируется и не перемещается: симулятор хранит ссылку на книгу движка.
 */
class MarketMakerEngine {
public:
    /// Обработчик исполнений лимитных заявок.
    using FillCallback = std::function<void(const Fill&)>;
    /// Обработчик рыночных заявок.
    using TakerCallback = std::function<void(const MarketFill&)>;

    /// @throws std::invalid_argument при недопустимой конфигурации
    explicit MarketMakerEngine(const EngineConfig& config);
    MarketMakerEngine(const MarketMakerEngine&) = delete;
    MarketMakerEngine& operator=(const MarketMakerEngine&) = delete;
    MarketMakerEngine(MarketMakerEngine&&) = delete;
    MarketMakerEngine& operator=(MarketMakerEngine&&) = delete;
    ~MarketMakerEngine() = default;

    /// Обновление лучших цен (`bookTicker`).
    void on_book(const BookTop& book);

    /// Сделка; возвращает число исполнений наших заявок.
    std::size_t on_trade(const Trade& trade);

    /// Снимок стакана.
    void on_depth_snapshot(const DepthSnapshot& snapshot);

    /// Инкрементальное обновление стакана.
    void on_depth_update(const DepthUpdate& update);

    /// Разобранное сообщение Binance.
    void on_message(const ParsedMessage& message);

    /// Закрыть позицию рыночной заявкой сейчас (проскальзывание по книге).
    std::optional<MarketFill> liquidate();

    /// Обработчик исполнений.
    void set_fill_callback(FillCallback callback) { on_fill_ = std::move(callback); }

    /// Обработчик рыночных заявок.
    void set_taker_callback(TakerCallback callback) { on_taker_ = std::move(callback); }

    /// Текущее состояние.
    [[nodiscard]] EngineStats stats() const;

    /// Время последнего события, мс.
    [[nodiscard]] std::int64_t now_ms() const noexcept { return now_ms_; }

    /// \f$ \tau \f$ в момент `ts_ms` при скользящем горизонте.
    [[nodiscard]] double tau_at(std::int64_t ts_ms) const noexcept;

    /// Конфигурация.
    [[nodiscard]] const EngineConfig& config() const noexcept { return config_; }

    /// Локальная книга заявок.
    [[nodiscard]] const OrderBook& order_book() const noexcept { return book_; }

    /// Виртуальная биржа.
    [[nodiscard]] const PaperExchange& exchange() const noexcept { return exchange_; }

private:
    void advance_clock(std::int64_t ts_ms) noexcept;
    void on_price_update();
    void check_session();
    void requote();
    [[nodiscard]] std::optional<BookTop> current_top() const;
    [[nodiscard]] double level_qty(Side side, double price) const;
    void track_inventory() noexcept;

    EngineConfig config_;
    EwmaVolatility vol_;
    IntensityEstimator intensity_;
    OrderBook book_;
    PaperExchange exchange_;
    FillCallback on_fill_;
    TakerCallback on_taker_;
    BookTop top_;
    bool has_top_ = false;
    Quotes quotes_;
    double tau_ = 0.0;
    double k_used_ = 0.0;
    double edge_ = 0.0;
    double max_abs_q_ = 0.0;
    std::int64_t now_ms_ = 0;
    std::int64_t session_start_ms_ = -1;
    std::size_t session_ = 0;
    std::size_t book_updates_ = 0;
    std::size_t depth_updates_ = 0;
    std::size_t depth_snapshots_ = 0;
    std::size_t depth_gaps_ = 0;
    std::size_t trades_ = 0;
    std::size_t taker_orders_ = 0;
    std::vector<LevelChange> changes_;
};

}  // namespace asmm
