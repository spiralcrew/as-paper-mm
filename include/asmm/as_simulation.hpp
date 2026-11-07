/**
 * @file as_simulation.hpp
 * @brief Воспроизведение численного эксперимента из §4 статьи Avellaneda–Stoikov (2008).
 *
 * Mid-цена — биномиальное блуждание: за шаг \f$ dt \f$ сдвиг \f$ \pm\sigma\sqrt{dt} \f$ с
 * вероятностью 1/2. На каждом шаге агент котирует бид/аск; заявка на расстоянии
 * \f$ \delta \f$ исполняется с вероятностью \f$ \lambda(\delta)\,dt = A e^{-k\delta} dt \f$.
 * Сравниваются две стратегии:
 * - **inventory** — котировки вокруг резервационной цены \f$ r = s - q\gamma\sigma^2(T-t) \f$;
 * - **symmetric** — тот же спред, но вокруг mid \f$ s \f$.
 *
 * Прибыль — \f$ X_T + q_T S_T \f$. Обе стратегии видят одну и ту же траекторию цены и
 * одни и те же случайные числа (common random numbers).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace asmm {

/// Параметры эксперимента (по умолчанию — как в статье, §4).
struct AsSimParams {
    double s0 = 100.0;         ///< начальная цена
    double horizon = 1.0;      ///< \f$ T \f$
    double sigma = 2.0;        ///< \f$ \sigma \f$
    double dt = 0.005;         ///< шаг
    double gamma = 0.1;        ///< \f$ \gamma \f$
    double k = 1.5;            ///< \f$ k \f$
    double A = 140.0;          ///< \f$ A \f$
    std::size_t paths = 1000;  ///< число траекторий
    std::uint64_t seed = 42;   ///< зерно ГСЧ
    /// Включать в спред член \f$ \gamma\sigma^2(T-t) \f$ (формула 3.18). Таблицы 1–3 статьи
    /// показывают постоянный спред \f$ \frac{2}{\gamma}\ln(1+\gamma/k) \f$ — чтобы сравнить с
    /// ними, этот член можно отключить.
    bool spread_time_term = true;
};

/// Статистика стратегии по траекториям.
struct AsStrategyStats {
    double mean_spread = 0.0;   ///< средний котируемый спред
    double mean_profit = 0.0;   ///< средняя прибыль
    double std_profit = 0.0;    ///< стандартное отклонение прибыли
    double mean_final_q = 0.0;  ///< средний конечный инвентарь
    double std_final_q = 0.0;   ///< стандартное отклонение конечного инвентаря
};

/// Результат эксперимента.
struct AsSimResult {
    AsStrategyStats inventory;  ///< стратегия вокруг резервационной цены
    AsStrategyStats symmetric;  ///< симметричная стратегия вокруг mid
};

/// Запустить эксперимент.
/// @throws std::invalid_argument при недопустимых параметрах
[[nodiscard]] AsSimResult run_as_simulation(const AsSimParams& params);

}  // namespace asmm
