/**
 * @file paper_exchange.cpp
 * @brief Реализация симулятора биржи.
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

const SimOrder kNoOrder{};

}  // namespace

PaperExchange::PaperExchange(const ExchangeConfig& config) : config_(config) {}

PaperExchange::PaperExchange(FillModel model, double maker_fee_rate) {
    config_.fill_model = model;
    config_.maker_fee_rate = maker_fee_rate;
}

SimOrder* PaperExchange::working(Side side) noexcept {
    for (auto it = orders_.rbegin(); it != orders_.rend(); ++it) {
        if (it->side == side && it->cancel_ms == SimOrder::kNever && it->active()) {
            return &*it;
        }
    }
    return nullptr;
}

const SimOrder& PaperExchange::bid() const noexcept {
    for (auto it = orders_.rbegin(); it != orders_.rend(); ++it) {
        if (it->side == Side::Buy && it->cancel_ms == SimOrder::kNever && it->active()) {
            return *it;
        }
    }
    return kNoOrder;
}

const SimOrder& PaperExchange::ask() const noexcept {
    for (auto it = orders_.rbegin(); it != orders_.rend(); ++it) {
        if (it->side == Side::Sell && it->cancel_ms == SimOrder::kNever && it->active()) {
            return *it;
        }
    }
    return kNoOrder;
}

void PaperExchange::activate(SimOrder& order) {
    order.live = true;
    if (config_.fill_model == FillModel::Queue && level_source_) {
        // Встаём в хвост: перед нами весь видимый объём уровня.
        order.level_qty = std::max(0.0, level_source_(order.side, order.price));
        order.queue_ahead = order.level_qty;
    }
}

void PaperExchange::erase_done(std::int64_t now_ms) {
    std::erase_if(orders_,
                  [now_ms](const SimOrder& o) { return !o.active() || o.cancel_ms <= now_ms; });
}

void PaperExchange::advance_to(std::int64_t now_ms) {
    for (auto& o : orders_) {
        if (o.cancel_ms <= now_ms) {
            continue;  // отмена дошла раньше, чем заявка встала бы в книгу
        }
        if (!o.live && o.live_ms <= now_ms) {
            activate(o);
        }
    }
    erase_done(now_ms);
}

void PaperExchange::place_side(Side side, std::optional<double> price, double qty,
                               std::int64_t now_ms) {
    SimOrder* current = working(side);
    const std::int64_t effective = now_ms + config_.latency_ms;
    if (price && qty > kQtyEps) {
        if (current != nullptr && std::abs(current->price - *price) <= kPriceEps) {
            return;  // та же цена — сохраняем место в очереди
        }
        if (current != nullptr) {
            current->cancel_ms = effective;
        }
        SimOrder order;
        order.id = next_id_++;
        order.side = side;
        order.price = *price;
        order.qty = qty;
        order.placed_ms = now_ms;
        order.live_ms = effective;
        orders_.push_back(order);
    } else if (current != nullptr) {
        current->cancel_ms = effective;
    }
}

void PaperExchange::set_quotes(std::optional<double> bid_price, std::optional<double> ask_price,
                               double bid_qty, double ask_qty, std::int64_t now_ms) {
    place_side(Side::Buy, bid_price, bid_qty, now_ms);
    place_side(Side::Sell, ask_price, ask_qty, now_ms);
    advance_to(now_ms);
}

void PaperExchange::cancel_all(std::int64_t now_ms) {
    set_quotes(std::nullopt, std::nullopt, 0.0, 0.0, now_ms);
}

std::vector<Fill> PaperExchange::on_trade(const Trade& trade) {
    std::vector<Fill> fills;
    advance_to(trade.ts_ms);
    if (!(trade.qty > 0.0)) {
        return fills;
    }
    // Агрессор-продавец (buyer is maker) бьёт в биды, агрессор-покупатель — в аски.
    const Side hit = trade.buyer_is_maker ? Side::Buy : Side::Sell;
    double available = trade.qty;
    for (auto& o : orders_) {
        if (!o.live || o.side != hit || !o.active()) {
            continue;
        }
        // > 0: сделка прошла хуже нашей цены (уровень пробит), 0: по нашей цене.
        const double through_by = hit == Side::Buy ? o.price - trade.price : trade.price - o.price;
        const bool through = through_by > kPriceEps;
        const bool at_price = std::abs(through_by) <= kPriceEps;
        if (!through && !at_price) {
            continue;
        }
        if (through) {
            // Агрессор прошёл наш уровень (price-time priority): вся ликвидность на нашей
            // цене, включая нас, уже исполнена — во всех моделях заявка исполняется целиком.
            o.queue_ahead = 0.0;
            fills.push_back(execute(o, o.qty, trade.ts_ms));
            continue;
        }
        // Сделка ровно по нашей цене — здесь модели и различаются.
        double qty = 0.0;
        switch (config_.fill_model) {
            case FillModel::Touch:
                qty = std::min(o.qty, available);
                break;
            case FillModel::Through:
                break;
            case FillModel::Queue: {
                const double consumed = std::min(o.queue_ahead, available);
                o.queue_ahead -= consumed;
                available -= consumed;
                o.level_qty = std::max(0.0, o.level_qty - trade.qty);
                qty = std::min(o.qty, available);
                break;
            }
        }
        if (qty > kQtyEps) {
            fills.push_back(execute(o, qty, trade.ts_ms));
            available = std::max(0.0, available - qty);
        }
    }
    erase_done(trade.ts_ms);
    return fills;
}

void PaperExchange::on_level_update(Side side, double price, double qty) {
    if (config_.fill_model != FillModel::Queue) {
        return;
    }
    qty = std::max(0.0, qty);
    for (auto& o : orders_) {
        if (!o.live || o.side != side || std::abs(o.price - price) > kPriceEps) {
            continue;
        }
        if (qty < o.level_qty && config_.cancel_model == CancelModel::Proportional &&
            o.level_qty > 0.0) {
            // Уменьшение объёма без сделок — отмены, распределённые равномерно по очереди.
            const double cancelled = o.level_qty - qty;
            o.queue_ahead -= cancelled * (o.queue_ahead / o.level_qty);
        }
        o.level_qty = qty;
        o.queue_ahead = std::clamp(o.queue_ahead, 0.0, qty);
    }
}

void PaperExchange::refresh_levels() {
    if (config_.fill_model != FillModel::Queue || !level_source_) {
        return;
    }
    for (auto& o : orders_) {
        if (o.live) {
            const double qty = level_source_(o.side, o.price);
            o.level_qty = std::max(0.0, qty);
            o.queue_ahead = std::clamp(o.queue_ahead, 0.0, o.level_qty);
        }
    }
}

MarketFill PaperExchange::market_order(Side side, double qty, const std::vector<PriceLevel>& levels,
                                       double mid, std::int64_t now_ms) {
    MarketFill mf;
    mf.side = side;
    mf.mid = mid;
    mf.ts_ms = now_ms;
    if (!(qty > kQtyEps)) {
        return mf;
    }
    if (levels.empty()) {
        mf.complete = false;
        return mf;
    }
    double remaining = qty;
    double notional = 0.0;
    double last_price = levels.front().price;
    for (const auto& level : levels) {
        if (remaining <= kQtyEps) {
            break;
        }
        const double take = std::min(remaining, level.qty);
        if (take <= kQtyEps) {
            continue;
        }
        notional += take * level.price;
        remaining -= take;
        last_price = level.price;
        ++mf.levels;
    }
    if (remaining > kQtyEps) {
        mf.complete = false;
        notional += remaining * last_price;
    }
    mf.qty = qty;
    mf.vwap = notional / qty;
    mf.fee = notional * config_.taker_fee_rate;
    mf.slippage = (side == Side::Buy ? mf.vwap - mid : mid - mf.vwap) * qty;
    if (side == Side::Buy) {
        position_ += qty;
        cash_ -= notional;
    } else {
        position_ -= qty;
        cash_ += notional;
    }
    cash_ -= mf.fee;
    fees_ += mf.fee;
    taker_volume_ += notional;
    slippage_ += mf.slippage;
    return mf;
}

Fill PaperExchange::execute(SimOrder& order, double qty, std::int64_t ts_ms) {
    const double notional = order.price * qty;
    const double fee = notional * config_.maker_fee_rate;
    if (order.side == Side::Buy) {
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
    return Fill{order.side, order.price, qty, fee, ts_ms, order.id};
}

}  // namespace asmm
