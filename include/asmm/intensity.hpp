/**
 * @file intensity.hpp
 * @brief Онлайн-калибровка интенсивности исполнения \f$ \lambda(\delta) = A e^{-k\delta} \f$.
 *
 * В Avellaneda–Stoikov (2008, §2.3) заявка на расстоянии \f$ \delta \f$ от mid исполняется
 * пуассоновским потоком с интенсивностью \f$ \lambda(\delta) = A e^{-k\delta} \f$; параметр
 * \f$ k \f$ входит в оптимальный спред. Здесь \f$ A \f$ и \f$ k \f$ оцениваются по ленте сделок:
 *
 * 1. время режется на интервалы длины \f$ \Delta \f$; в начале интервала фиксируется
 *    mid \f$ s_0 \f$ (цена, от которой выставлены котировки);
 * 2. за интервал запоминается максимальная глубина, до которой дошли агрессоры:
 *    \f$ D^a = \max(p - s_0) \f$ по покупкам, \f$ D^b = \max(s_0 - p) \f$ по продажам;
 * 3. заявка на глубине \f$ \delta \f$ исполнилась бы в интервале, если \f$ D \ge \delta \f$,
 *    поэтому \f$ \hat P(\delta) = \#\{D \ge \delta\}/N \f$, а для пуассоновского потока
 *    \f$ \hat\lambda(\delta) = -\ln(1 - \hat P(\delta)) / \Delta \f$;
 * 4. МНК по сетке глубин: \f$ \ln\hat\lambda(\delta_i) = \ln A - k\,\delta_i \f$.
 *
 * Сетка глубин — \f$ G \f$ квантилей положительных глубин окна, от мелкой до самой глубокой,
 * на которой ещё есть `min_hits` попаданий; точки совпадают со скачками эмпирической функции
 * выживания (глубины дискретны: кратны половине тика). Оценка считается пригодной, только если
 * \f$ R^2 \ge \f$ `min_r2`: на реальном BTCUSDT распределение глубин бывает двугорбым (касание
 * лучшей цены и редкие скачки цены), и экспонента тогда описывает его плохо — движок в этом
 * случае использует заданный `k`.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

#include "asmm/types.hpp"

namespace asmm {

/// Параметры оценщика интенсивности.
struct IntensityConfig {
    double bucket_sec = 1.0;        ///< \f$ \Delta \f$, длина интервала, с
    std::size_t window = 900;       ///< число последних интервалов в окне
    std::size_t grid_points = 15;   ///< \f$ G \f$, число точек сетки глубин
    std::size_t min_samples = 120;  ///< минимум наблюдений (2 на интервал) для оценки
    std::size_t min_hits = 5;       ///< минимум попаданий в точке сетки
    double min_r2 = 0.6;            ///< минимальный R², иначе оценка не используется
};

/// Результат калибровки.
struct IntensityFit {
    double A = 0.0;          ///< \f$ A \f$, 1/с
    double k = 0.0;          ///< \f$ k \f$, 1/USDT
    double r2 = 0.0;         ///< \f$ R^2 \f$ регрессии \f$ \ln\lambda \f$ на \f$ \delta \f$
    std::size_t points = 0;  ///< точек в регрессии
    bool valid = false;      ///< оценка пригодна (≥ 3 точек, k > 0, R² ≥ min_r2)
};

/// Онлайн-оценщик \f$ A \f$ и \f$ k \f$ по ленте сделок и mid-цене.
class IntensityEstimator {
public:
    /// @throws std::invalid_argument при недопустимых параметрах
    explicit IntensityEstimator(const IntensityConfig& config = {});

    /// Новая mid-цена в момент `ts_ms`; при необходимости закрывает интервал.
    void on_mid(std::int64_t ts_ms, double mid);

    /// Публичная сделка.
    void on_trade(const Trade& trade);

    /// Добавить готовое наблюдение глубины (для тестов и офлайн-калибровки).
    void add_sample(double depth);

    /// Пересчитать оценку по текущему окну.
    const IntensityFit& refit();

    /// Последняя оценка.
    [[nodiscard]] const IntensityFit& fit() const noexcept { return fit_; }

    /// Число наблюдений в окне.
    [[nodiscard]] std::size_t samples() const noexcept { return samples_.size(); }

private:
    void roll(std::int64_t ts_ms);
    void close_bucket();

    IntensityConfig config_;
    std::deque<double> samples_;
    IntensityFit fit_;
    bool open_ = false;
    std::int64_t bucket_end_ms_ = 0;
    double ref_mid_ = 0.0;
    double last_mid_ = 0.0;
    double bid_depth_ = 0.0;
    double ask_depth_ = 0.0;
};

}  // namespace asmm
