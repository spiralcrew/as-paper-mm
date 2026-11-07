/**
 * @file as_simulate.cpp
 * @brief CLI `asmm_sim`: воспроизведение численного эксперимента Avellaneda–Stoikov (2008), §4.
 *
 * @code
 * ./build/asmm_sim               # γ = 0.01, 0.1, 0.5; 1000 траекторий
 * ./build/asmm_sim --paths 5000 --seed 7
 * @endcode
 */
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>

#include "asmm/as_simulation.hpp"

int main(int argc, char** argv) {
    asmm::AsSimParams base;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--paths" && i + 1 < argc) {
                base.paths = static_cast<std::size_t>(std::stoul(argv[++i]));
            } else if (arg == "--seed" && i + 1 < argc) {
                base.seed = std::stoull(argv[++i]);
            } else {
                std::puts("Usage: asmm_sim [--paths N] [--seed S]");
                return arg == "-h" || arg == "--help" ? 0 : 2;
            }
        }
        std::printf(
            "Avellaneda & Stoikov (2008), section 4: s=%.0f T=%.0f sigma=%.0f dt=%.3f k=%.1f "
            "A=%.0f, "
            "%zu paths\n\n",
            base.s0, base.horizon, base.sigma, base.dt, base.k, base.A, base.paths);
        for (const bool time_term : {true, false}) {
            std::puts(
                time_term
                    ? "Spread = gamma*sigma^2*(T-t) + (2/gamma)*ln(1+gamma/k)  (eq. 3.18)"
                    : "\nSpread = (2/gamma)*ln(1+gamma/k), constant  (as reported in Tables 1-3)");
            std::printf("%-6s %-10s %8s %9s %9s %9s %9s\n", "gamma", "strategy", "spread", "profit",
                        "std(pnl)", "final q", "std(q)");
            for (const double gamma : {0.01, 0.1, 0.5}) {
                asmm::AsSimParams p = base;
                p.gamma = gamma;
                p.spread_time_term = time_term;
                const asmm::AsSimResult r = asmm::run_as_simulation(p);
                const auto row = [gamma](const char* name, const asmm::AsStrategyStats& s) {
                    std::printf("%-6.2f %-10s %8.2f %9.2f %9.2f %9.2f %9.2f\n", gamma, name,
                                s.mean_spread, s.mean_profit, s.std_profit, s.mean_final_q,
                                s.std_final_q);
                };
                row("inventory", r.inventory);
                row("symmetric", r.symmetric);
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
