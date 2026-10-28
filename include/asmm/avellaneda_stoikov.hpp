/**
 * @file avellaneda_stoikov.hpp
 * @brief Модель маркет-мейкинга Avellaneda–Stoikov (2008).
 *
 * Avellaneda M., Stoikov S. High-frequency trading in a limit order book //
 * Quantitative Finance. 2008. Vol. 8, No. 3. P. 217–224.
 *
 * Обозначения:
 * - \f$ s \f$ — mid-цена, USDT;
 * - \f$ q \f$ — инвентарь в лотах (позиция / размер заявки), со знаком;
 * - \f$ \gamma \f$ — коэффициент неприятия риска, 1/USDT;
 * - \f$ \sigma \f$ — волатильность mid-цены, USDT/\f$\sqrt{s}\f$;
 * - \f$ \tau = T - t \f$ — время до конца торговой сессии, секунды;
 * - \f$ k \f$ — параметр интенсивности исполнения \f$ \lambda(\delta) = A e^{-k\delta} \f$, 1/USDT.
 */
#pragma once

#include "asmm/types.hpp"

namespace asmm {

/// Параметры модели Avellaneda–Stoikov.
struct AsParams {
    double gamma = 0.001;       ///< \f$ \gamma \f$, неприятие риска, 1/USDT
    double k = 2.0;             ///< \f$ k \f$, затухание интенсивности исполнения, 1/USDT
    double horizon_sec = 30.0;  ///< \f$ T \f$, длина торговой сессии, секунды
};

/// Котировки модели.
struct Quotes {
    double reservation_price = 0.0;  ///< \f$ r \f$, резервационная цена
    double spread = 0.0;             ///< \f$ \delta^a + \delta^b \f$, полный оптимальный спред
    double bid = 0.0;                ///< \f$ r - \text{spread}/2 \f$
    double ask = 0.0;                ///< \f$ r + \text{spread}/2 \f$
};

/**
 * @brief Резервационная (indifference) цена маркет-мейкера.
 *
 * \f[ r = s - q \gamma \sigma^2 \tau \f]
 *
 * @throws std::invalid_argument при mid <= 0, gamma <= 0, sigma < 0, tau < 0.
 */
[[nodiscard]] double reservation_price(double mid, double inventory, double gamma, double sigma,
                                       double tau);

/**
 * @brief Оптимальный полный спред вокруг резервационной цены.
 *
 * \f[ \delta^a + \delta^b = \gamma \sigma^2 \tau + \frac{2}{\gamma}\ln\left(1 +
 * \frac{\gamma}{k}\right) \f]
 *
 * @throws std::invalid_argument при gamma <= 0, k <= 0, sigma < 0, tau < 0.
 */
[[nodiscard]] double optimal_spread(double gamma, double sigma, double tau, double k);

/**
 * @brief Сырые (неокруглённые) котировки бид/аск по модели.
 *
 * @param mid       mid-цена \f$ s \f$
 * @param inventory инвентарь \f$ q \f$ в лотах
 * @param sigma     волатильность \f$ \sigma \f$, USDT/\f$\sqrt{s}\f$
 * @param tau       время до конца сессии \f$ \tau \f$, секунды
 * @param params    \f$ \gamma, k \f$
 */
[[nodiscard]] Quotes compute_quotes(double mid, double inventory, double sigma, double tau,
                                    const AsParams& params);

/**
 * @brief Округлить котировки к сетке цен и сделать их post-only.
 *
 * Бид округляется вниз, аск — вверх к шагу цены `tick_size`. Затем бид
 * ограничивается `best_ask - tick`, аск — `best_bid + tick`, чтобы заявки
 * не пересекали стакан (мы только мейкер).
 *
 * @throws std::invalid_argument при tick_size <= 0 или невалидном стакане.
 */
[[nodiscard]] Quotes snap_to_book(const Quotes& raw, const BookTop& book, double tick_size);

/// Округлить цену к ближайшему узлу сетки `tick_size`.
[[nodiscard]] double round_to_tick(double price, double tick_size) noexcept;

}  // namespace asmm
