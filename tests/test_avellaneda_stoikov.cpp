#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>

#include "asmm/avellaneda_stoikov.hpp"

namespace asmm {
namespace {

TEST(ReservationPrice, EqualsMidWithZeroInventory) {
    EXPECT_DOUBLE_EQ(reservation_price(100.0, 0.0, 0.1, 2.0, 10.0), 100.0);
}

TEST(ReservationPrice, LongInventoryShiftsDown) {
    // r = s - q*gamma*sigma^2*tau = 100 - 2*0.1*4*10 = 92
    EXPECT_DOUBLE_EQ(reservation_price(100.0, 2.0, 0.1, 2.0, 10.0), 92.0);
}

TEST(ReservationPrice, ShortInventoryShiftsUp) {
    EXPECT_DOUBLE_EQ(reservation_price(100.0, -2.0, 0.1, 2.0, 10.0), 108.0);
}

TEST(ReservationPrice, NoSkewAtHorizonEnd) {
    EXPECT_DOUBLE_EQ(reservation_price(100.0, 5.0, 0.1, 2.0, 0.0), 100.0);
}

TEST(OptimalSpread, MatchesClosedForm) {
    const double gamma = 0.1, sigma = 2.0, tau = 10.0, k = 1.5;
    const double expected = gamma * sigma * sigma * tau + (2.0 / gamma) * std::log(1.0 + gamma / k);
    EXPECT_NEAR(optimal_spread(gamma, sigma, tau, k), expected, 1e-12);
    EXPECT_NEAR(optimal_spread(gamma, sigma, tau, k), 5.290770, 1e-6);
}

TEST(OptimalSpread, SmallGammaLimitIsTwoOverK) {
    // gamma -> 0: (2/gamma) ln(1 + gamma/k) -> 2/k, риск-член исчезает.
    EXPECT_NEAR(optimal_spread(1e-9, 1.0, 1.0, 2.0), 1.0, 1e-6);
}

TEST(OptimalSpread, GrowsWithVolatilityAndHorizon) {
    EXPECT_LT(optimal_spread(0.1, 1.0, 10.0, 1.5), optimal_spread(0.1, 2.0, 10.0, 1.5));
    EXPECT_LT(optimal_spread(0.1, 2.0, 1.0, 1.5), optimal_spread(0.1, 2.0, 10.0, 1.5));
}

TEST(ComputeQuotes, SymmetricAroundReservationPrice) {
    const AsParams p{0.1, 1.5, 10.0};
    const Quotes q = compute_quotes(100.0, 1.0, 2.0, 10.0, p);
    EXPECT_DOUBLE_EQ(q.reservation_price, 96.0);
    EXPECT_NEAR(q.ask - q.reservation_price, q.reservation_price - q.bid, 1e-12);
    EXPECT_NEAR(q.ask - q.bid, q.spread, 1e-12);
}

TEST(ComputeQuotes, RejectsInvalidParameters) {
    const AsParams ok{0.1, 1.5, 10.0};
    EXPECT_THROW((void)compute_quotes(0.0, 0.0, 1.0, 1.0, ok), std::invalid_argument);
    EXPECT_THROW((void)compute_quotes(100.0, 0.0, -1.0, 1.0, ok), std::invalid_argument);
    EXPECT_THROW((void)compute_quotes(100.0, 0.0, 1.0, -1.0, ok), std::invalid_argument);
    EXPECT_THROW((void)compute_quotes(100.0, 0.0, 1.0, 1.0, AsParams{0.0, 1.5, 10.0}),
                 std::invalid_argument);
    EXPECT_THROW((void)compute_quotes(100.0, 0.0, 1.0, 1.0, AsParams{0.1, 0.0, 10.0}),
                 std::invalid_argument);
}

TEST(SnapToBook, RoundsOutwardToTick) {
    const BookTop book{99.00, 1.0, 101.00, 1.0, 1, 0};
    Quotes raw;
    raw.bid = 99.537;
    raw.ask = 100.461;
    const Quotes q = snap_to_book(raw, book, 0.01);
    EXPECT_NEAR(q.bid, 99.53, 1e-9);
    EXPECT_NEAR(q.ask, 100.47, 1e-9);
}

TEST(SnapToBook, KeepsQuotesPostOnly) {
    // Сильный шорт: r выше рынка, бид не должен пересекать лучший аск.
    const BookTop book{100.00, 1.0, 100.01, 1.0, 1, 0};
    Quotes raw;
    raw.bid = 105.0;
    raw.ask = 106.0;
    const Quotes q = snap_to_book(raw, book, 0.01);
    EXPECT_NEAR(q.bid, 100.00, 1e-9);
    EXPECT_NEAR(q.ask, 106.00, 1e-9);
    EXPECT_LT(q.bid, book.ask_price);
}

TEST(SnapToBook, ExactTickValuesAreStable) {
    const BookTop book{80467.81, 1.0, 80467.82, 1.0, 1, 0};
    Quotes raw;
    raw.bid = 80467.81;
    raw.ask = 80467.82;
    const Quotes q = snap_to_book(raw, book, 0.01);
    EXPECT_NEAR(q.bid, 80467.81, 1e-7);
    EXPECT_NEAR(q.ask, 80467.82, 1e-7);
}

}  // namespace
}  // namespace asmm
