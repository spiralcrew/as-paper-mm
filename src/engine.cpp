/**
 * @file engine.cpp
 * @brief Реализация движка paper market making.
 */
#include "asmm/engine.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace asmm {

namespace {

const EngineConfig& validated(const EngineConfig& c) {
    if (!(c.tick_size > 0.0)) {
        throw std::invalid_argument("tick_size must be positive");
    }
    if (!(c.order_qty > 0.0)) {
        throw std::invalid_argument("order_qty must be positive");
    }
    if (!(c.max_inventory_lots >= 0.0)) {
        throw std::invalid_argument("max_inventory_lots must be non-negative");
    }
    if (!(c.as.horizon_sec > 0.0)) {
        throw std::invalid_argument("horizon_sec must be positive");
    }
    if (!(c.as.gamma > 0.0) || !(c.as.k > 0.0)) {
        throw std::invalid_argument("gamma and k must be positive");
    }
    return c;
}

}  // namespace

MarketMakerEngine::MarketMakerEngine(const EngineConfig& config)
    : config_(validated(config)),
      vol_(config.vol_half_life_sec, config.initial_sigma, config.vol_min_interval_ms),
      exchange_(config.fill_model, config.maker_fee_rate),
      tau_(config.as.horizon_sec) {}

void MarketMakerEngine::advance_clock(std::int64_t ts_ms) noexcept {
    if (ts_ms > now_ms_) {
        now_ms_ = ts_ms;
    }
    if (session_start_ms_ < 0 && now_ms_ > 0) {
        session_start_ms_ = now_ms_;
    }
}

double MarketMakerEngine::tau_at(std::int64_t ts_ms) const noexcept {
    const double horizon = config_.as.horizon_sec;
    if (session_start_ms_ < 0 || ts_ms < session_start_ms_) {
        return horizon;
    }
    const double elapsed = static_cast<double>(ts_ms - session_start_ms_) / 1000.0;
    return horizon - std::fmod(elapsed, horizon);
}

void MarketMakerEngine::on_book(const BookTop& book) {
    if (!book.valid()) {
        return;
    }
    advance_clock(book.ts_ms);
    book_ = book;
    has_book_ = true;
    ++book_updates_;
    vol_.update(now_ms_, book.mid());
    requote();
}

std::size_t MarketMakerEngine::on_trade(const Trade& trade) {
    advance_clock(trade.ts_ms);
    ++trades_;
    const auto fill = exchange_.on_trade(trade);
    if (!fill) {
        return 0;
    }
    if (on_fill_) {
        on_fill_(*fill);
    }
    if (has_book_) {
        requote();
    }
    return 1;
}

void MarketMakerEngine::on_message(const ParsedMessage& message) {
    switch (message.type) {
        case MessageType::BookTicker:
            on_book(message.book);
            break;
        case MessageType::Trade:
            on_trade(message.trade);
            break;
    }
}

void MarketMakerEngine::requote() {
    const double q_lots = exchange_.position() / config_.order_qty;
    tau_ = tau_at(now_ms_);
    const Quotes raw = compute_quotes(book_.mid(), q_lots, vol_.sigma(), tau_, config_.as);
    quotes_ = snap_to_book(raw, book_, config_.tick_size);

    // Лимит инвентаря: объём стороны урезается до остатка лимита |q| <= max_inventory_lots,
    // поэтому даже частичные исполнения не выводят позицию за лимит.
    const double max_pos = config_.max_inventory_lots * config_.order_qty;
    const double position = exchange_.position();
    const double bid_qty = std::clamp(max_pos - position, 0.0, config_.order_qty);
    const double ask_qty = std::clamp(max_pos + position, 0.0, config_.order_qty);
    std::optional<double> bid = quotes_.bid;
    std::optional<double> ask = quotes_.ask;
    if (quotes_.bid <= 0.0) {
        bid.reset();
    }
    exchange_.set_quotes(bid, ask, bid_qty, ask_qty);
}

EngineStats MarketMakerEngine::stats() const {
    EngineStats s;
    s.ts_ms = now_ms_;
    s.mid = has_book_ ? book_.mid() : 0.0;
    s.sigma = vol_.sigma();
    s.tau = tau_;
    s.reservation_price = quotes_.reservation_price;
    s.bid = exchange_.bid().price;
    s.ask = exchange_.ask().price;
    s.bid_active = exchange_.bid().active();
    s.ask_active = exchange_.ask().active();
    s.inventory_btc = exchange_.position();
    s.inventory_lots = exchange_.position() / config_.order_qty;
    s.cash = exchange_.cash();
    s.equity = has_book_ ? exchange_.equity(book_.mid()) : exchange_.cash();
    s.fees = exchange_.fees();
    s.volume_quote = exchange_.volume_quote();
    s.fills = exchange_.fill_count();
    s.book_updates = book_updates_;
    s.trades = trades_;
    return s;
}

}  // namespace asmm
