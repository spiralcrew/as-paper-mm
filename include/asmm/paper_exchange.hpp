/**
 * @file paper_exchange.hpp
 * @brief Симулятор исполнения лимитных заявок по ленте публичных сделок (paper trading).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "asmm/types.hpp"

namespace asmm {

/**
 * @brief Правило, по которому публичная сделка исполняет нашу виртуальную заявку.
 *
 * Мы не видим свою позицию в очереди, поэтому используем две границы:
 * - `Touch` — оптимистичная: сделка по нашей цене исполняет нас (считаем, что мы первые в очереди);
 * - `Through` — пессимистичная: нужна сделка строго хуже нашей цены (уровень «пробит» насквозь).
 */
enum class FillModel : std::uint8_t {
    Touch,   ///< исполнение при касании цены
    Through  ///< исполнение только при пробое цены
};

/// Виртуальное исполнение нашей заявки.
struct Fill {
    Side side = Side::Buy;   ///< сторона нашей заявки
    double price = 0.0;      ///< цена исполнения (цена нашей заявки), USDT
    double qty = 0.0;        ///< исполненный объём, BTC
    double fee = 0.0;        ///< комиссия, USDT (отрицательная — ребейт)
    std::int64_t ts_ms = 0;  ///< время сделки, вызвавшей исполнение
};

/// Виртуальная лимитная заявка, стоящая в книге.
struct RestingOrder {
    double price = 0.0;  ///< цена, USDT
    double qty = 0.0;    ///< неисполненный остаток, BTC

    /// Заявка активна, пока есть неисполненный остаток.
    [[nodiscard]] bool active() const noexcept { return qty > 0.0; }
};

/**
 * @brief Виртуальная биржа: держит по одной заявке на сторону, исполняет их по ленте
 * сделок и ведёт учёт позиции, кэша и комиссий. Реальные деньги не используются.
 *
 * Правило: сделка с агрессором-продавцом (`buyer_is_maker == true`) может исполнить
 * наш бид, с агрессором-покупателем — наш аск. Исполняемый объём —
 * `min(остаток заявки, объём сделки)`, частичные исполнения поддерживаются.
 */
class PaperExchange {
public:
    /**
     * @param model          правило исполнения
     * @param maker_fee_rate комиссия мейкера как доля от notional (0.001 = 0.1%)
     */
    explicit PaperExchange(FillModel model = FillModel::Touch, double maker_fee_rate = 0.0);

    /**
     * @brief Выставить/переставить котировки. `std::nullopt` снимает сторону.
     *
     * Если цена стороны не изменилась и заявка ещё активна, остаток сохраняется
     * (частично исполненная заявка не «доливается»).
     */
    void set_quotes(std::optional<double> bid_price, std::optional<double> ask_price,
                    double bid_qty, double ask_qty);

    /// То же, с одинаковым объёмом на обеих сторонах.
    void set_quotes(std::optional<double> bid_price, std::optional<double> ask_price, double qty) {
        set_quotes(bid_price, ask_price, qty, qty);
    }

    /// Снять обе заявки.
    void cancel_all() noexcept;

    /// Обработать публичную сделку. Возвращает исполнение, если наша заявка задета.
    std::optional<Fill> on_trade(const Trade& trade);

    [[nodiscard]] const RestingOrder& bid() const noexcept { return bid_; }  ///< текущий бид
    [[nodiscard]] const RestingOrder& ask() const noexcept { return ask_; }  ///< текущий аск
    [[nodiscard]] double position() const noexcept { return position_; }     ///< позиция, BTC
    [[nodiscard]] double cash() const noexcept { return cash_; }  ///< кэш, USDT (старт 0)
    [[nodiscard]] double fees() const noexcept { return fees_; }  ///< уплачено комиссий, USDT
    [[nodiscard]] std::size_t fill_count() const noexcept { return fills_; }  ///< число исполнений
    [[nodiscard]] double volume_quote() const noexcept { return volume_; }    ///< оборот, USDT

    /// Mark-to-market капитал: кэш + позиция по цене `mark`.
    [[nodiscard]] double equity(double mark) const noexcept { return cash_ + position_ * mark; }

private:
    Fill execute(RestingOrder& order, Side side, const Trade& trade);

    FillModel model_;
    double fee_rate_;
    RestingOrder bid_;
    RestingOrder ask_;
    double position_ = 0.0;
    double cash_ = 0.0;
    double fees_ = 0.0;
    double volume_ = 0.0;
    std::size_t fills_ = 0;
};

}  // namespace asmm
