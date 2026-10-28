/**
 * @file volatility.cpp
 * @brief Реализация EWMA-оценки волатильности.
 */
#include "asmm/volatility.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace asmm {

EwmaVolatility::EwmaVolatility(double half_life_sec, double initial_sigma,
                               std::int64_t min_interval_ms)
    : half_life_sec_(half_life_sec),
      var_(initial_sigma * initial_sigma),
      min_interval_ms_(min_interval_ms) {
    if (!(half_life_sec > 0.0)) {
        throw std::invalid_argument("half_life_sec must be positive");
    }
    if (!(initial_sigma >= 0.0)) {
        throw std::invalid_argument("initial_sigma must be non-negative");
    }
    if (min_interval_ms < 0) {
        throw std::invalid_argument("min_interval_ms must be non-negative");
    }
}

void EwmaVolatility::update(std::int64_t ts_ms, double price) noexcept {
    if (!std::isfinite(price)) {
        return;
    }
    if (!has_anchor_) {
        anchor_ts_ = ts_ms;
        anchor_price_ = price;
        has_anchor_ = true;
        return;
    }
    const std::int64_t elapsed_ms = ts_ms - anchor_ts_;
    if (elapsed_ms <= 0 || elapsed_ms < min_interval_ms_) {
        return;
    }
    const double dt = static_cast<double>(elapsed_ms) / 1000.0;
    const double dx = price - anchor_price_;
    const double alpha = 1.0 - std::exp(-std::numbers::ln2 * dt / half_life_sec_);
    var_ = (1.0 - alpha) * var_ + alpha * (dx * dx / dt);
    anchor_ts_ = ts_ms;
    anchor_price_ = price;
    ++samples_;
}

double EwmaVolatility::sigma() const noexcept {
    return std::sqrt(var_);
}

}  // namespace asmm
