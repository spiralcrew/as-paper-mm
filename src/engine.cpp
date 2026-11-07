/**
 * @file engine.cpp
 * @brief Реализация движка paper market making.
 */
#include "asmm/engine.hpp"

#include <algorithm>
#include <cmath>
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
    if (!(c.k_min > 0.0) || !(c.k_max >= c.k_min)) {
        throw std::invalid_argument("require 0 < k_min <= k_max");
    }
    if (c.exchange.latency_ms < 0) {
        throw std::invalid_argument("latency_ms must be non-negative");
    }
    return c;
}

/// Допуск сравнения цен.
constexpr double kPriceEps = 1e-8;
/// Позиция меньше этого считается нулевой, BTC.
constexpr double kPositionEps = 1e-12;
/// Сколько уровней стакана доступно рыночной заявке.
constexpr std::size_t kLiquidationDepth = 200;

}  // namespace

MarketMakerEngine::MarketMakerEngine(const EngineConfig& config)
    : config_(validated(config)),
      vol_(config.vol_half_life_sec, config.initial_sigma, config.vol_min_interval_ms),
      intensity_(config.intensity),
      book_(config.tick_size),
      exchange_(config.exchange),
      tau_(config.as.horizon_sec),
      k_used_(config.as.k) {
    exchange_.set_level_source([this](Side side, double price) { return level_qty(side, price); });
}

double MarketMakerEngine::level_qty(Side side, double price) const {
    if (book_.synced()) {
        return book_.qty_at(side, price);
    }
    if (!has_top_) {
        return 0.0;
    }
    // Без полной книги известен только объём лучших уровней.
    if (side == Side::Buy && std::abs(price - top_.bid_price) <= kPriceEps) {
        return top_.bid_qty;
    }
    if (side == Side::Sell && std::abs(price - top_.ask_price) <= kPriceEps) {
        return top_.ask_qty;
    }
    return 0.0;
}

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

std::optional<BookTop> MarketMakerEngine::current_top() const {
    if (has_top_) {
        return top_;
    }
    if (book_.synced()) {
        return book_.top();
    }
    return std::nullopt;
}

void MarketMakerEngine::track_inventory() noexcept {
    max_abs_q_ = std::max(max_abs_q_, std::abs(exchange_.position() / config_.order_qty));
}

void MarketMakerEngine::on_book(const BookTop& book) {
    if (!book.valid()) {
        return;
    }
    advance_clock(book.ts_ms);
    exchange_.advance_to(now_ms_);
    top_ = book;
    has_top_ = true;
    ++book_updates_;
    if (!book_.synced()) {
        exchange_.on_level_update(Side::Buy, book.bid_price, book.bid_qty);
        exchange_.on_level_update(Side::Sell, book.ask_price, book.ask_qty);
    }
    on_price_update();
}

void MarketMakerEngine::on_depth_snapshot(const DepthSnapshot& snapshot) {
    advance_clock(snapshot.ts_ms);
    exchange_.advance_to(now_ms_);
    book_.apply_snapshot(snapshot);
    ++depth_snapshots_;
    exchange_.refresh_levels();
    if (!has_top_) {
        on_price_update();
    }
}

void MarketMakerEngine::on_depth_update(const DepthUpdate& update) {
    advance_clock(update.ts_ms);
    exchange_.advance_to(now_ms_);
    changes_.clear();
    switch (book_.apply_update(update, &changes_)) {
        case BookUpdateResult::Applied:
            ++depth_updates_;
            for (const auto& c : changes_) {
                exchange_.on_level_update(c.side, c.price, c.qty);
            }
            if (!has_top_) {
                on_price_update();
            }
            break;
        case BookUpdateResult::Gap:
            ++depth_gaps_;
            break;
        case BookUpdateResult::Stale:
        case BookUpdateResult::NoSnapshot:
            break;
    }
}

void MarketMakerEngine::on_price_update() {
    const auto top = current_top();
    if (!top || !top->valid()) {
        return;
    }
    vol_.update(now_ms_, top->mid());
    intensity_.on_mid(now_ms_, top->mid());
    check_session();
    requote();
}

std::size_t MarketMakerEngine::on_trade(const Trade& trade) {
    advance_clock(trade.ts_ms);
    ++trades_;
    Trade t = trade;
    t.ts_ms = now_ms_;
    intensity_.on_trade(t);
    const std::vector<Fill> fills = exchange_.on_trade(t);
    if (fills.empty()) {
        return 0;
    }
    const auto top = current_top();
    const double mid = top ? top->mid() : 0.0;
    for (const auto& f : fills) {
        if (mid > 0.0) {
            edge_ += (f.side == Side::Buy ? mid - f.price : f.price - mid) * f.qty;
        }
        if (on_fill_) {
            on_fill_(f);
        }
    }
    track_inventory();
    requote();
    return fills.size();
}

void MarketMakerEngine::on_message(const ParsedMessage& message) {
    switch (message.type) {
        case MessageType::BookTicker:
            on_book(message.book);
            break;
        case MessageType::Trade:
            on_trade(message.trade);
            break;
        case MessageType::DepthUpdate:
            on_depth_update(message.depth);
            break;
        case MessageType::DepthSnapshot:
            on_depth_snapshot(message.snapshot);
            break;
    }
}

void MarketMakerEngine::check_session() {
    if (session_start_ms_ < 0) {
        return;
    }
    const auto horizon_ms = config_.as.horizon_sec * 1000.0;
    const auto index =
        static_cast<std::size_t>(static_cast<double>(now_ms_ - session_start_ms_) / horizon_ms);
    if (index > session_) {
        session_ = index;
        if (config_.liquidate_at_session_end) {
            liquidate();
        }
    }
}

std::optional<MarketFill> MarketMakerEngine::liquidate() {
    const double position = exchange_.position();
    const auto top = current_top();
    if (std::abs(position) <= kPositionEps || !top || !top->valid()) {
        return std::nullopt;
    }
    // Продажа съедает биды, покупка — аски.
    const Side side = position > 0.0 ? Side::Sell : Side::Buy;
    const Side book_side = side == Side::Sell ? Side::Buy : Side::Sell;
    std::vector<PriceLevel> levels;
    if (book_.synced()) {
        levels = book_.top_levels(book_side, kLiquidationDepth);
    } else if (side == Side::Sell) {
        levels.push_back(PriceLevel{top->bid_price, top->bid_qty});
    } else {
        levels.push_back(PriceLevel{top->ask_price, top->ask_qty});
    }
    const MarketFill fill =
        exchange_.market_order(side, std::abs(position), levels, top->mid(), now_ms_);
    if (fill.qty > 0.0) {
        ++taker_orders_;
        if (on_taker_) {
            on_taker_(fill);
        }
    }
    return fill;
}

void MarketMakerEngine::requote() {
    const auto top = current_top();
    if (!top || !top->valid()) {
        return;
    }
    const double mid = top->mid();
    const double position = exchange_.position();
    const double q_lots = position / config_.order_qty;
    tau_ = tau_at(now_ms_);

    const IntensityFit& fit = intensity_.fit();
    k_used_ = config_.calibrate_k && fit.valid ? std::clamp(fit.k, config_.k_min, config_.k_max)
                                               : config_.as.k;
    AsParams params = config_.as;
    params.k = k_used_;
    const Quotes raw = compute_quotes(mid, q_lots, vol_.sigma(), tau_, params);
    quotes_ = snap_to_book(raw, *top, config_.tick_size);

    // Лимит инвентаря: объём стороны урезается до остатка лимита |q| <= max_inventory_lots.
    const double max_pos = config_.max_inventory_lots * config_.order_qty;
    const double bid_qty = std::clamp(max_pos - position, 0.0, config_.order_qty);
    const double ask_qty = std::clamp(max_pos + position, 0.0, config_.order_qty);

    std::optional<double> bid = quotes_.bid;
    std::optional<double> ask = quotes_.ask;
    if (quotes_.bid <= 0.0) {
        bid.reset();
    }

    // Гистерезис: не переставляем заявку из-за сдвига меньше requote_ticks — сохраняем очередь.
    const double threshold = static_cast<double>(config_.requote_ticks) * config_.tick_size;
    const SimOrder& wb = exchange_.bid();
    if (bid && wb.active() && std::abs(*bid - wb.price) < threshold - kPriceEps &&
        wb.price < top->ask_price - kPriceEps) {
        bid = wb.price;
    }
    const SimOrder& wa = exchange_.ask();
    if (ask && wa.active() && std::abs(*ask - wa.price) < threshold - kPriceEps &&
        wa.price > top->bid_price + kPriceEps) {
        ask = wa.price;
    }
    exchange_.set_quotes(bid, ask, bid_qty, ask_qty, now_ms_);
}

EngineStats MarketMakerEngine::stats() const {
    EngineStats s;
    const auto top = current_top();
    const SimOrder& bid = exchange_.bid();
    const SimOrder& ask = exchange_.ask();
    s.ts_ms = now_ms_;
    s.mid = top ? top->mid() : 0.0;
    s.sigma = vol_.sigma();
    s.tau = tau_;
    s.k = k_used_;
    s.intensity_A = intensity_.fit().valid ? intensity_.fit().A : 0.0;
    s.k_calibrated = config_.calibrate_k && intensity_.fit().valid;
    s.k_fit = intensity_.fit().valid ? intensity_.fit().k : 0.0;
    s.k_fit_r2 = intensity_.fit().r2;
    s.reservation_price = quotes_.reservation_price;
    s.spread = quotes_.spread;
    s.bid = bid.price;
    s.ask = ask.price;
    s.bid_active = bid.active();
    s.ask_active = ask.active();
    s.bid_queue = bid.queue_ahead;
    s.ask_queue = ask.queue_ahead;
    s.inventory_btc = exchange_.position();
    s.inventory_lots = exchange_.position() / config_.order_qty;
    s.max_abs_inventory_lots = max_abs_q_;
    s.cash = exchange_.cash();
    s.equity = s.mid > 0.0 ? exchange_.equity(s.mid) : exchange_.cash();
    s.fees = exchange_.fees();
    s.volume_quote = exchange_.volume_quote();
    s.taker_volume = exchange_.taker_volume();
    s.slippage_cost = exchange_.slippage_cost();
    s.edge_captured = edge_;
    s.fills = exchange_.fill_count();
    s.taker_orders = taker_orders_;
    s.book_updates = book_updates_;
    s.depth_updates = depth_updates_;
    s.depth_snapshots = depth_snapshots_;
    s.depth_gaps = depth_gaps_;
    s.book_synced = book_.synced();
    s.trades = trades_;
    s.session = session_;
    return s;
}

}  // namespace asmm
