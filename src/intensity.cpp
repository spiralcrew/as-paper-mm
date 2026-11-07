/**
 * @file intensity.cpp
 * @brief Реализация калибровки \f$ \lambda(\delta) = A e^{-k\delta} \f$.
 */
#include "asmm/intensity.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace asmm {

namespace {

/// Глубина интервала, в котором на стороне не было сделок.
constexpr double kNoHit = -std::numeric_limits<double>::infinity();

}  // namespace

IntensityEstimator::IntensityEstimator(const IntensityConfig& config) : config_(config) {
    if (!(config.bucket_sec > 0.0) || config.window == 0 || config.grid_points < 3 ||
        config.min_hits == 0) {
        throw std::invalid_argument("invalid intensity estimator config");
    }
}

void IntensityEstimator::roll(std::int64_t ts_ms) {
    if (open_ && ts_ms >= bucket_end_ms_) {
        close_bucket();
    }
    if (!open_ && last_mid_ > 0.0) {
        open_ = true;
        ref_mid_ = last_mid_;
        bid_depth_ = kNoHit;
        ask_depth_ = kNoHit;
        bucket_end_ms_ =
            ts_ms + static_cast<std::int64_t>(std::llround(config_.bucket_sec * 1000.0));
    }
}

void IntensityEstimator::on_mid(std::int64_t ts_ms, double mid) {
    if (!(mid > 0.0)) {
        return;
    }
    if (open_ && ts_ms >= bucket_end_ms_) {
        close_bucket();
    }
    last_mid_ = mid;
    roll(ts_ms);
}

void IntensityEstimator::on_trade(const Trade& trade) {
    roll(trade.ts_ms);
    if (!open_) {
        return;
    }
    if (trade.buyer_is_maker) {
        bid_depth_ = std::max(bid_depth_, ref_mid_ - trade.price);
    } else {
        ask_depth_ = std::max(ask_depth_, trade.price - ref_mid_);
    }
}

void IntensityEstimator::close_bucket() {
    open_ = false;
    samples_.push_back(bid_depth_);
    samples_.push_back(ask_depth_);
    while (samples_.size() > 2 * config_.window) {
        samples_.pop_front();
    }
    refit();
}

void IntensityEstimator::add_sample(double depth) {
    samples_.push_back(depth);
    while (samples_.size() > 2 * config_.window) {
        samples_.pop_front();
    }
}

const IntensityFit& IntensityEstimator::refit() {
    IntensityFit fit;
    const std::size_t n = samples_.size();
    if (n < config_.min_samples) {
        fit_ = fit;
        return fit_;
    }
    std::vector<double> sorted(samples_.begin(), samples_.end());
    std::sort(sorted.begin(), sorted.end());
    const auto first_positive = std::upper_bound(sorted.begin(), sorted.end(), 0.0);
    const auto positives = static_cast<std::size_t>(sorted.end() - first_positive);
    if (positives < config_.min_hits) {
        fit_ = fit;
        return fit_;
    }
    // Сетка — квантили положительных глубин от мелкой до самой глубокой, где ещё есть
    // min_hits попаданий. Точки сетки совпадают со скачками эмпирической функции выживания,
    // что важно на дискретной ценовой сетке (глубины кратны половине тика).
    const std::size_t last = positives - config_.min_hits;
    double sx = 0.0;
    double sy = 0.0;
    double sxx = 0.0;
    double sxy = 0.0;
    double syy = 0.0;
    std::size_t m = 0;
    double prev_delta = -1.0;
    for (std::size_t j = 0; j < config_.grid_points; ++j) {
        const auto idx = static_cast<std::size_t>(
            std::llround(static_cast<double>(j) * static_cast<double>(last) /
                         static_cast<double>(config_.grid_points - 1)));
        const double delta = *(first_positive + static_cast<std::ptrdiff_t>(idx));
        if (delta <= prev_delta + 1e-12) {
            continue;
        }
        prev_delta = delta;
        const auto hits = static_cast<std::size_t>(
            sorted.end() - std::lower_bound(sorted.begin(), sorted.end(), delta));
        if (hits < config_.min_hits || hits >= n) {
            continue;
        }
        const double p = static_cast<double>(hits) / static_cast<double>(n);
        const double lambda = -std::log1p(-p) / config_.bucket_sec;
        const double y = std::log(lambda);
        sx += delta;
        sy += y;
        sxx += delta * delta;
        sxy += delta * y;
        syy += y * y;
        ++m;
    }
    if (m < 3) {
        fit_ = fit;
        return fit_;
    }
    const auto md = static_cast<double>(m);
    const double cov = sxy - sx * sy / md;
    const double var_x = sxx - sx * sx / md;
    const double var_y = syy - sy * sy / md;
    if (!(var_x > 0.0)) {
        fit_ = fit;
        return fit_;
    }
    const double slope = cov / var_x;
    const double intercept = (sy - slope * sx) / md;
    fit.k = -slope;
    fit.A = std::exp(intercept);
    fit.r2 = var_y > 0.0 ? (cov * cov) / (var_x * var_y) : 1.0;
    fit.points = m;
    fit.valid =
        fit.k > 0.0 && std::isfinite(fit.k) && std::isfinite(fit.A) && fit.r2 >= config_.min_r2;
    fit_ = fit;
    return fit_;
}

}  // namespace asmm
