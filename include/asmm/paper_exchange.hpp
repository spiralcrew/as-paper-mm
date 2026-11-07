/**
 * @file paper_exchange.hpp
 * @brief Симулятор биржи для paper trading: очередь лимитных заявок, задержка, отмены,
 * рыночные заявки с проскальзыванием по стакану.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "asmm/types.hpp"

namespace asmm {

/**
 * @brief Правило, по которому публичная сделка исполняет нашу виртуальную заявку.
 *
 * - `Queue` (по умолчанию) — моделируется место в очереди уровня (price-time priority):
 *   встаём в хвост за видимым объёмом уровня; сделки по нашей цене сначала съедают
 *   очередь перед нами, затем нас; сделка хуже нашей цены означает, что уровень
 *   пробит целиком, и заявка исполняется полностью;
 * - `Touch` — оптимистичная граница: сделка по нашей цене исполняет нас сразу;
 * - `Through` — пессимистичная граница: нужна сделка строго хуже нашей цены.
 */
enum class FillModel : std::uint8_t {
    Queue,   ///< очередь уровня
    Touch,   ///< исполнение при касании цены
    Through  ///< исполнение только при пробое цены
};

/**
 * @brief Как уменьшение объёма уровня без сделок (отмены чужих заявок) двигает нашу очередь.
 *
 * - `Proportional` — отменяется равномерно по очереди: объём перед нами уменьшается
 *   пропорционально его доле в уровне;
 * - `Pessimistic` — отменяются только заявки за нами; очередь перед нами уменьшают лишь
 *   сделки (и ограничение «не больше объёма уровня»).
 */
enum class CancelModel : std::uint8_t {
    Proportional,  ///< пропорционально доле
    Pessimistic    ///< отмены только позади нас
};

/// Параметры симулятора биржи.
struct ExchangeConfig {
    FillModel fill_model = FillModel::Queue;               ///< правило исполнения
    CancelModel cancel_model = CancelModel::Proportional;  ///< модель отмен
    double maker_fee_rate = 0.0;    ///< комиссия мейкера (доля notional), 0.001 = 0.1%
    double taker_fee_rate = 0.001;  ///< комиссия тейкера (Binance Spot VIP0 — 0.1%)
    std::int64_t latency_ms = 0;    ///< задержка выставления/отмены заявки, мс
};

/// Исполнение нашей заявки.
struct Fill {
    Side side = Side::Buy;       ///< сторона нашей заявки
    double price = 0.0;          ///< цена исполнения, USDT
    double qty = 0.0;            ///< объём, BTC
    double fee = 0.0;            ///< комиссия, USDT (отрицательная — ребейт)
    std::int64_t ts_ms = 0;      ///< время исполнения
    std::uint64_t order_id = 0;  ///< id виртуальной заявки
};

/// Виртуальная лимитная заявка.
struct SimOrder {
    /// Время «никогда» для отмены.
    static constexpr std::int64_t kNever = std::numeric_limits<std::int64_t>::max();

    std::uint64_t id = 0;             ///< id заявки
    Side side = Side::Buy;            ///< сторона
    double price = 0.0;               ///< цена, USDT
    double qty = 0.0;                 ///< неисполненный остаток, BTC
    double queue_ahead = 0.0;         ///< объём чужих заявок перед нами на уровне, BTC
    double level_qty = 0.0;           ///< последний известный объём уровня (без нас), BTC
    std::int64_t placed_ms = 0;       ///< когда отправлена
    std::int64_t live_ms = 0;         ///< когда встала в книгу (placed + latency)
    std::int64_t cancel_ms = kNever;  ///< когда снимется (отмена + latency)
    bool live = false;                ///< уже в книге и может исполняться

    /// Заявка ещё не исполнена полностью.
    [[nodiscard]] bool active() const noexcept { return qty > 0.0; }
};

/// Совместимое имя для рабочей заявки стороны.
using RestingOrder = SimOrder;

/// Результат рыночной (тейкер) заявки.
struct MarketFill {
    Side side = Side::Buy;   ///< сторона
    double qty = 0.0;        ///< исполненный объём, BTC
    double vwap = 0.0;       ///< средняя цена, USDT
    double mid = 0.0;        ///< mid в момент заявки
    double slippage = 0.0;   ///< издержки относительно mid: |vwap − mid| · qty, USDT
    double fee = 0.0;        ///< комиссия тейкера, USDT
    std::size_t levels = 0;  ///< сколько уровней стакана съедено
    bool complete = true;    ///< видимой ликвидности хватило
    std::int64_t ts_ms = 0;  ///< время
};

/// Источник видимого объёма уровня: (сторона книги, цена) → объём.
using LevelQtyFn = std::function<double(Side, double)>;

/**
 * @brief Виртуальная биржа: держит наши лимитные заявки, исполняет их по ленте сделок
 * с учётом очереди и задержки, исполняет рыночные заявки по стакану и ведёт учёт
 * позиции, кэша и комиссий. Реальные деньги не используются.
 *
 * Жизненный цикл заявки: `set_quotes` в момент t → заявка становится живой в
 * t + latency (тогда же фиксируется очередь перед ней); замена/отмена старой заявки
 * вступает в силу в t + latency, до этого старая заявка может исполниться
 * (риск «устаревшей котировки»).
 *
 * Допущение: наши заявки не влияют на рынок (no market impact).
 */
class PaperExchange {
public:
    /// Симулятор с заданной конфигурацией.
    explicit PaperExchange(const ExchangeConfig& config = {});

    /// Упрощённый конструктор: правило исполнения и комиссия мейкера, без задержки.
    PaperExchange(FillModel model, double maker_fee_rate);

    /// Источник объёма уровней для модели очереди (по умолчанию очередь считается пустой).
    void set_level_source(LevelQtyFn source) { level_source_ = std::move(source); }

    /// Продвинуть время: активировать заявки, у которых прошла задержка, снять отменённые.
    void advance_to(std::int64_t now_ms);

    /**
     * @brief Выставить/переставить котировки в момент `now_ms`. `std::nullopt` снимает сторону.
     *
     * Если цена стороны не изменилась, заявка сохраняет место в очереди и остаток
     * (частично исполненная заявка не «доливается»).
     */
    void set_quotes(std::optional<double> bid_price, std::optional<double> ask_price,
                    double bid_qty, double ask_qty, std::int64_t now_ms = 0);

    /// То же, с одинаковым объёмом на обеих сторонах и моментом 0.
    void set_quotes(std::optional<double> bid_price, std::optional<double> ask_price, double qty) {
        set_quotes(bid_price, ask_price, qty, qty, 0);
    }

    /// Снять обе заявки (с учётом задержки).
    void cancel_all(std::int64_t now_ms = 0);

    /// Обработать публичную сделку; возвращает исполнения наших заявок.
    std::vector<Fill> on_trade(const Trade& trade);

    /// Новый объём уровня книги (из `depthUpdate`/`bookTicker`) — двигает очередь.
    void on_level_update(Side side, double price, double qty);

    /// Перечитать объём уровней всех живых заявок из источника (после нового снимка книги).
    void refresh_levels();

    /**
     * @brief Рыночная заявка: проход по уровням противоположной стороны.
     * @param levels уровни, которые съедаются (для продажи — биды), от лучшего к худшему
     * @param mid    mid в момент заявки — база для проскальзывания
     *
     * Если видимой ликвидности не хватило, остаток исполняется по худшей видимой цене
     * и `complete == false`.
     */
    MarketFill market_order(Side side, double qty, const std::vector<PriceLevel>& levels,
                            double mid, std::int64_t now_ms);

    [[nodiscard]] const SimOrder& bid() const noexcept;  ///< рабочая (последняя) заявка на покупку
    [[nodiscard]] const SimOrder& ask() const noexcept;  ///< рабочая (последняя) заявка на продажу
    [[nodiscard]] const std::vector<SimOrder>& orders() const noexcept { return orders_; }  ///< все

    [[nodiscard]] double position() const noexcept { return position_; }  ///< позиция, BTC
    [[nodiscard]] double cash() const noexcept { return cash_; }          ///< кэш, USDT (старт 0)
    [[nodiscard]] double fees() const noexcept { return fees_; }          ///< комиссии, USDT
    [[nodiscard]] std::size_t fill_count() const noexcept { return fills_; }  ///< мейкер-исполнений
    [[nodiscard]] double volume_quote() const noexcept { return volume_; }  ///< мейкер-оборот, USDT
    [[nodiscard]] double taker_volume() const noexcept { return taker_volume_; }  ///< тейкер-оборот
    [[nodiscard]] double slippage_cost() const noexcept { return slippage_; }  ///< издержки тейкера
    [[nodiscard]] const ExchangeConfig& config() const noexcept { return config_; }  ///< параметры

    /// Mark-to-market капитал: кэш + позиция по цене `mark`.
    [[nodiscard]] double equity(double mark) const noexcept { return cash_ + position_ * mark; }

private:
    void place_side(Side side, std::optional<double> price, double qty, std::int64_t now_ms);
    void activate(SimOrder& order);
    void erase_done(std::int64_t now_ms);
    SimOrder* working(Side side) noexcept;
    Fill execute(SimOrder& order, double qty, std::int64_t ts_ms);

    ExchangeConfig config_;
    LevelQtyFn level_source_;
    std::vector<SimOrder> orders_;
    std::uint64_t next_id_ = 1;
    double position_ = 0.0;
    double cash_ = 0.0;
    double fees_ = 0.0;
    double volume_ = 0.0;
    double taker_volume_ = 0.0;
    double slippage_ = 0.0;
    std::size_t fills_ = 0;
};

}  // namespace asmm
