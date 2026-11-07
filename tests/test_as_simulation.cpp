#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>

#include "asmm/as_simulation.hpp"

namespace asmm {
namespace {

TEST(AsSimulation, AverageSpreadMatchesFormula) {
    // Спред детерминирован: среднее по шагам γσ²(T − t_i) + (2/γ) ln(1 + γ/k).
    AsSimParams p;
    p.paths = 10;
    const AsSimResult r = run_as_simulation(p);
    const double mean_tau = 1.0 - 0.005 * 99.5;
    const double expected = 0.1 * 4.0 * mean_tau + 20.0 * std::log1p(0.1 / 1.5);
    EXPECT_NEAR(r.inventory.mean_spread, expected, 1e-9);
}

TEST(AsSimulation, InventoryStrategyReducesRisk) {
    // Главный вывод статьи: inventory-стратегия даёт сопоставимую прибыль при намного меньшей
    // дисперсии прибыли и конечного инвентаря, чем симметричная.
    const AsSimResult r = run_as_simulation(AsSimParams{});
    EXPECT_LT(r.inventory.std_profit, 0.6 * r.symmetric.std_profit);
    EXPECT_LT(r.inventory.std_final_q, 0.5 * r.symmetric.std_final_q);
    EXPECT_GT(r.inventory.mean_profit, 0.85 * r.symmetric.mean_profit);
}

TEST(AsSimulation, ReproducesPaperTable1) {
    // Таблица 1 статьи (γ = 0.1, 1000 траекторий, постоянный спред 1.29):
    // inventory 62.94 ± 5.89, std(q) 2.80; symmetric 67.21 ± 13.43, std(q) 8.66.
    AsSimParams p;
    p.spread_time_term = false;
    p.paths = 4000;
    const AsSimResult r = run_as_simulation(p);
    EXPECT_NEAR(r.inventory.mean_spread, 1.29, 0.005);
    EXPECT_NEAR(r.inventory.mean_profit, 62.94, 2.5);
    EXPECT_NEAR(r.inventory.std_profit, 5.89, 0.6);
    EXPECT_NEAR(r.inventory.std_final_q, 2.80, 0.3);
    EXPECT_NEAR(r.symmetric.mean_profit, 67.21, 2.5);
    EXPECT_NEAR(r.symmetric.std_profit, 13.43, 1.0);
    EXPECT_NEAR(r.symmetric.std_final_q, 8.66, 0.6);
}

TEST(AsSimulation, IsDeterministicForSeed) {
    AsSimParams p;
    p.paths = 50;
    const AsSimResult a = run_as_simulation(p);
    const AsSimResult b = run_as_simulation(p);
    EXPECT_DOUBLE_EQ(a.inventory.mean_profit, b.inventory.mean_profit);
}

TEST(AsSimulation, RejectsInvalidParameters) {
    AsSimParams p;
    p.paths = 1;
    EXPECT_THROW((void)run_as_simulation(p), std::invalid_argument);
}

}  // namespace
}  // namespace asmm
