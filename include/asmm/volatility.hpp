/**
 * @file volatility.hpp
 * @brief Онлайн-оценка волатильности mid-цены (EWMA по приращениям).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace asmm {

/**
 * @brief EWMA-оценка дисперсии приращений цены в единицу времени.
 *
 * На каждом сэмпле \f$ \Delta x = s_t - s_{t-\Delta t} \f$:
 * \f[ \hat v \leftarrow (1-\alpha)\hat v + \alpha \frac{\Delta x^2}{\Delta t}, \quad
 *     \alpha = 1 - 2^{-\Delta t / h} \f]
 * где \f$ h \f$ — период полураспада. Возвращает \f$ \sigma = \sqrt{\hat v} \f$ в
 * USDT/\f$\sqrt{s}\f$ — ровно та единица, что нужна в модели Avellaneda–Stoikov.
 *
 * Сэмплы реже `min_interval_ms` пропускаются: на тиковой частоте mid-цена
 * содержит шум микроструктуры (bid–ask bounce), который завышает оценку
 * (Zhang, Mykland, Aït-Sahalia, 2005, "A Tale of Two Time Scales").
 */
class EwmaVolatility {
public:
    /**
     * @param half_life_sec   период полураспада весов, секунды (> 0)
     * @param initial_sigma   стартовая оценка σ до накопления данных (>= 0)
     * @param min_interval_ms минимальный шаг между сэмплами, мс (>= 0)
     * @throws std::invalid_argument при недопустимых параметрах
     */
    explicit EwmaVolatility(double half_life_sec, double initial_sigma = 0.0,
                            std::int64_t min_interval_ms = 1000);

    /// Добавить наблюдение цены в момент `ts_ms`. Наблюдения из прошлого игнорируются.
    void update(std::int64_t ts_ms, double price) noexcept;

    /// Текущая оценка σ, USDT/√s.
    [[nodiscard]] double sigma() const noexcept;

    /// Текущая оценка дисперсии в секунду, USDT²/s.
    [[nodiscard]] double variance_per_sec() const noexcept { return var_; }

    /// Число учтённых приращений.
    [[nodiscard]] std::size_t samples() const noexcept { return samples_; }

private:
    double half_life_sec_;
    double var_;
    std::int64_t min_interval_ms_;
    std::int64_t anchor_ts_ = 0;
    double anchor_price_ = 0.0;
    bool has_anchor_ = false;
    std::size_t samples_ = 0;
};

}  // namespace asmm
