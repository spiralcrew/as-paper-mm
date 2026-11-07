/**
 * @file order_book.cpp
 * @brief Реализация локальной L2-книги.
 */
#include "asmm/order_book.hpp"

#include <cmath>
#include <stdexcept>

namespace asmm {

namespace {

/// Объём меньше этого считается нулевым.
constexpr double kQtyEps = 1e-12;

}  // namespace

OrderBook::OrderBook(double tick_size) : tick_size_(tick_size) {
    if (!(tick_size > 0.0)) {
        throw std::invalid_argument("tick_size must be positive");
    }
}

std::int64_t OrderBook::to_ticks(double price) const noexcept {
    return std::llround(price / tick_size_);
}

double OrderBook::to_price(std::int64_t ticks) const noexcept {
    return static_cast<double>(ticks) * tick_size_;
}

void OrderBook::apply_snapshot(const DepthSnapshot& snapshot) {
    bids_.clear();
    asks_.clear();
    for (const auto& l : snapshot.bids) {
        if (l.qty > kQtyEps) {
            bids_[to_ticks(l.price)] = l.qty;
        }
    }
    for (const auto& l : snapshot.asks) {
        if (l.qty > kQtyEps) {
            asks_[to_ticks(l.price)] = l.qty;
        }
    }
    last_update_id_ = snapshot.last_update_id;
    ts_ms_ = snapshot.ts_ms;
    synced_ = true;
}

void OrderBook::set_level(Side side, double price, double qty, std::vector<LevelChange>* changes) {
    const std::int64_t key = to_ticks(price);
    const bool remove = !(qty > kQtyEps);
    if (side == Side::Buy) {
        if (remove) {
            bids_.erase(key);
        } else {
            bids_[key] = qty;
        }
    } else {
        if (remove) {
            asks_.erase(key);
        } else {
            asks_[key] = qty;
        }
    }
    if (changes != nullptr) {
        changes->push_back(LevelChange{side, to_price(key), remove ? 0.0 : qty});
    }
}

BookUpdateResult OrderBook::apply_update(const DepthUpdate& update,
                                         std::vector<LevelChange>* changes) {
    if (!synced_) {
        return BookUpdateResult::NoSnapshot;
    }
    if (update.final_update_id <= last_update_id_) {
        return BookUpdateResult::Stale;
    }
    if (update.first_update_id > last_update_id_ + 1) {
        synced_ = false;
        return BookUpdateResult::Gap;
    }
    for (const auto& l : update.bids) {
        set_level(Side::Buy, l.price, l.qty, changes);
    }
    for (const auto& l : update.asks) {
        set_level(Side::Sell, l.price, l.qty, changes);
    }
    last_update_id_ = update.final_update_id;
    if (update.ts_ms > ts_ms_) {
        ts_ms_ = update.ts_ms;
    }
    return BookUpdateResult::Applied;
}

void OrderBook::reset() noexcept {
    bids_.clear();
    asks_.clear();
    last_update_id_ = 0;
    synced_ = false;
}

std::optional<PriceLevel> OrderBook::best_bid() const noexcept {
    if (bids_.empty()) {
        return std::nullopt;
    }
    const auto& [ticks, qty] = *bids_.begin();
    return PriceLevel{to_price(ticks), qty};
}

std::optional<PriceLevel> OrderBook::best_ask() const noexcept {
    if (asks_.empty()) {
        return std::nullopt;
    }
    const auto& [ticks, qty] = *asks_.begin();
    return PriceLevel{to_price(ticks), qty};
}

std::optional<BookTop> OrderBook::top() const noexcept {
    const auto bid = best_bid();
    const auto ask = best_ask();
    if (!bid || !ask) {
        return std::nullopt;
    }
    return BookTop{bid->price, bid->qty, ask->price, ask->qty, last_update_id_, ts_ms_};
}

double OrderBook::qty_at(Side side, double price) const noexcept {
    const std::int64_t key = to_ticks(price);
    if (side == Side::Buy) {
        const auto it = bids_.find(key);
        return it == bids_.end() ? 0.0 : it->second;
    }
    const auto it = asks_.find(key);
    return it == asks_.end() ? 0.0 : it->second;
}

std::size_t OrderBook::levels(Side side) const noexcept {
    return side == Side::Buy ? bids_.size() : asks_.size();
}

std::vector<PriceLevel> OrderBook::top_levels(Side side, std::size_t n) const {
    std::vector<PriceLevel> out;
    out.reserve(n);
    auto collect = [&](const auto& levels) {
        for (const auto& [ticks, qty] : levels) {
            if (out.size() >= n) {
                break;
            }
            out.push_back(PriceLevel{to_price(ticks), qty});
        }
    };
    if (side == Side::Buy) {
        collect(bids_);
    } else {
        collect(asks_);
    }
    return out;
}

}  // namespace asmm
