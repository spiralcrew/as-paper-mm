/**
 * @file as_simulation.cpp
 * @brief Монте-Карло эксперимент Avellaneda–Stoikov (2008), §4.
 */
#include "asmm/as_simulation.hpp"

#include <cmath>
#include <random>
#include <stdexcept>
#include <vector>

#include "asmm/avellaneda_stoikov.hpp"

namespace asmm {

namespace {

struct Agent {
    double cash = 0.0;
    double q = 0.0;
};

/// Независимый от реализации stdlib перевод 64 бит ГСЧ в равномерное [0, 1).
double uniform01(std::mt19937_64& rng) {
    return static_cast<double>(rng() >> 11) * 0x1.0p-53;
}

void summarize(const std::vector<double>& profit, const std::vector<double>& final_q,
               double spread_sum, std::size_t spread_count, AsStrategyStats& out) {
    const auto n = static_cast<double>(profit.size());
    double mp = 0.0;
    double mq = 0.0;
    for (std::size_t i = 0; i < profit.size(); ++i) {
        mp += profit[i];
        mq += final_q[i];
    }
    mp /= n;
    mq /= n;
    double vp = 0.0;
    double vq = 0.0;
    for (std::size_t i = 0; i < profit.size(); ++i) {
        vp += (profit[i] - mp) * (profit[i] - mp);
        vq += (final_q[i] - mq) * (final_q[i] - mq);
    }
    out.mean_profit = mp;
    out.std_profit = std::sqrt(vp / (n - 1.0));
    out.mean_final_q = mq;
    out.std_final_q = std::sqrt(vq / (n - 1.0));
    out.mean_spread = spread_sum / static_cast<double>(spread_count);
}

}  // namespace

AsSimResult run_as_simulation(const AsSimParams& p) {
    if (!(p.horizon > 0.0) || !(p.dt > 0.0) || p.dt > p.horizon || p.paths < 2 ||
        !(p.sigma >= 0.0) || !(p.A > 0.0) || !(p.s0 > 0.0)) {
        throw std::invalid_argument("invalid simulation parameters");
    }
    const auto steps = static_cast<std::size_t>(std::llround(p.horizon / p.dt));
    const double step = p.sigma * std::sqrt(p.dt);
    const AsParams as{p.gamma, p.k, p.horizon};

    std::mt19937_64 rng(p.seed);
    std::vector<double> profit_inv(p.paths);
    std::vector<double> profit_sym(p.paths);
    std::vector<double> q_inv(p.paths);
    std::vector<double> q_sym(p.paths);
    double spread_sum = 0.0;
    std::size_t spread_count = 0;

    for (std::size_t path = 0; path < p.paths; ++path) {
        double s = p.s0;
        Agent inv;
        Agent sym;
        for (std::size_t i = 0; i < steps; ++i) {
            const double tau = p.horizon - static_cast<double>(i) * p.dt;
            Quotes q = compute_quotes(s, inv.q, p.sigma, tau, as);
            if (!p.spread_time_term) {
                q.spread = optimal_spread(p.gamma, p.sigma, 0.0, p.k);
                q.bid = q.reservation_price - 0.5 * q.spread;
                q.ask = q.reservation_price + 0.5 * q.spread;
            }
            spread_sum += q.spread;
            ++spread_count;

            const double u_bid = uniform01(rng);
            const double u_ask = uniform01(rng);

            // Стратегия inventory: котировки вокруг r.
            if (u_bid < p.A * std::exp(-p.k * (s - q.bid)) * p.dt) {
                inv.q += 1.0;
                inv.cash -= q.bid;
            }
            if (u_ask < p.A * std::exp(-p.k * (q.ask - s)) * p.dt) {
                inv.q -= 1.0;
                inv.cash += q.ask;
            }
            // Симметричная стратегия: тот же спред вокруг mid.
            const double half = 0.5 * q.spread;
            if (u_bid < p.A * std::exp(-p.k * half) * p.dt) {
                sym.q += 1.0;
                sym.cash -= s - half;
            }
            if (u_ask < p.A * std::exp(-p.k * half) * p.dt) {
                sym.q -= 1.0;
                sym.cash += s + half;
            }

            s += uniform01(rng) < 0.5 ? step : -step;
        }
        profit_inv[path] = inv.cash + inv.q * s;
        profit_sym[path] = sym.cash + sym.q * s;
        q_inv[path] = inv.q;
        q_sym[path] = sym.q;
    }

    AsSimResult result;
    summarize(profit_inv, q_inv, spread_sum, spread_count, result.inventory);
    summarize(profit_sym, q_sym, spread_sum, spread_count, result.symmetric);
    return result;
}

}  // namespace asmm
