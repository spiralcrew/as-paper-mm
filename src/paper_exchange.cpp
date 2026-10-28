/**
 * @file paper_exchange.cpp
 * @brief Реализация симулятора исполнения.
 */
#include "asmm/paper_exchange.hpp"

#include <algorithm>
#include <cmath>

namespace asmm {

namespace {

/// Допуск сравнения цен: цены приходят строками и парсятся в double.
constexpr double kPriceEps = 1e-8;
/// Остаток меньше этого считается нулевым.
constexpr double kQtyEps = 1e-12;

void update_side(RestingOrder& order, std::optional<double> price, double qty) {
    if (!price) {
        order = RestingOrder{};
        return;
    }
    if (order.active() && std::abs(order.price - *price) <= kPriceEps) {
        return;
    }
    order.price = *price;
    order.qty = qty;
}

}  // namespace

PaperExchange::PaperExchange(FillModel model, double maker_fee_rate)
    : model_(model), fee_rate_(maker_fee_rate) {}

void PaperExchange::set_quotes(std::optional<double> bid_price, std::optional<double> ask_price,
                               double bid_qty, double ask_qty) {
    update_side(bid_, bid_qty > kQtyEps ? bid_price : std::nullopt, bid_qty);
    update_side(ask_, ask_qty > kQtyEps ? ask_price : std::nullopt, ask_qty);
}

void PaperExchange::cancel_all() noexcept {
    bid_ = RestingOrder{};
    ask_ = RestingOrder{};
}

std::optional<Fill> PaperExchange::on_trade(const Trade& trade) {
    if (!(trade.qty > 0.0)) {
        return std::nullopt;
    }
    const bool touch = model_ == FillModel::Touch;
    if (trade.buyer_is_maker) {
        // Агрессор продаёт по бидам: наш бид исполнен, если сделка по нашей цене или ниже.
        const bool hit =
            touch ? trade.price <= bid_.price + kPriceEps : trade.price < bid_.price - kPriceEps;
        if (bid_.active() && hit) {
            return execute(bid_, Side::Buy, trade);
        }
    } else {
        // Агрессор покупает по аскам: наш аск исполнен, если сделка по нашей цене или выше.
        const bool hit =
            touch ? trade.price >= ask_.price - kPriceEps : trade.price > ask_.price + kPriceEps;
        if (ask_.active() && hit) {
            return execute(ask_, Side::Sell, trade);
        }
    }
    return std::nullopt;
}

Fill PaperExchange::execute(RestingOrder& order, Side side, const Trade& trade) {
    const double qty = std::min(order.qty, trade.qty);
    const double notional = order.price * qty;
    const double fee = notional * fee_rate_;
    if (side == Side::Buy) {
        position_ += qty;
        cash_ -= notional;
    } else {
        position_ -= qty;
        cash_ += notional;
    }
    cash_ -= fee;
    fees_ += fee;
    volume_ += notional;
    ++fills_;
    order.qty -= qty;
    if (order.qty <= kQtyEps) {
        order.qty = 0.0;
    }
    return Fill{side, order.price, qty, fee, trade.ts_ms};
}

}  // namespace asmm
