/**
 * @file avellaneda_stoikov.cpp
 * @brief Реализация формул Avellaneda–Stoikov.
 */
#include "asmm/avellaneda_stoikov.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace asmm {

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::invalid_argument(message);
    }
}

}  // namespace

double reservation_price(double mid, double inventory, double gamma, double sigma, double tau) {
    require(std::isfinite(mid) && mid > 0.0, "mid must be positive and finite");
    require(std::isfinite(inventory), "inventory must be finite");
    require(gamma > 0.0, "gamma must be positive");
    require(sigma >= 0.0, "sigma must be non-negative");
    require(tau >= 0.0, "tau must be non-negative");
    return mid - inventory * gamma * sigma * sigma * tau;
}

double optimal_spread(double gamma, double sigma, double tau, double k) {
    require(gamma > 0.0, "gamma must be positive");
    require(k > 0.0, "k must be positive");
    require(sigma >= 0.0, "sigma must be non-negative");
    require(tau >= 0.0, "tau must be non-negative");
    return gamma * sigma * sigma * tau + (2.0 / gamma) * std::log1p(gamma / k);
}

Quotes compute_quotes(double mid, double inventory, double sigma, double tau,
                      const AsParams& params) {
    Quotes q;
    q.reservation_price = reservation_price(mid, inventory, params.gamma, sigma, tau);
    q.spread = optimal_spread(params.gamma, sigma, tau, params.k);
    q.bid = q.reservation_price - 0.5 * q.spread;
    q.ask = q.reservation_price + 0.5 * q.spread;
    return q;
}

double round_to_tick(double price, double tick_size) noexcept {
    return std::round(price / tick_size) * tick_size;
}

Quotes snap_to_book(const Quotes& raw, const BookTop& book, double tick_size) {
    require(tick_size > 0.0, "tick_size must be positive");
    require(book.valid(), "book must be valid");
    // Допуск против ошибок представления double (например, 80467.81 / 0.01).
    constexpr double kEps = 1e-9;
    Quotes q = raw;
    const double bid = std::floor(raw.bid / tick_size + kEps) * tick_size;
    const double ask = std::ceil(raw.ask / tick_size - kEps) * tick_size;
    q.bid = round_to_tick(std::min(bid, book.ask_price - tick_size), tick_size);
    q.ask = round_to_tick(std::max(ask, book.bid_price + tick_size), tick_size);
    return q;
}

}  // namespace asmm
