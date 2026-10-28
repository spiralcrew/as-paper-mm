#include <gtest/gtest.h>

#include <stdexcept>

#include "asmm/volatility.hpp"

namespace asmm {
namespace {

TEST(EwmaVolatility, StartsFromInitialSigma) {
    const EwmaVolatility v(10.0, 3.0);
    EXPECT_DOUBLE_EQ(v.sigma(), 3.0);
    EXPECT_EQ(v.samples(), 0U);
}

TEST(EwmaVolatility, ConstantPriceDecaysToZero) {
    EwmaVolatility v(5.0, 1.0, 0);
    for (int i = 0; i <= 300; ++i) {
        v.update(i * 1000, 100.0);
    }
    EXPECT_NEAR(v.sigma(), 0.0, 1e-6);
}

TEST(EwmaVolatility, ConvergesToKnownVariance) {
    // Шаг ±1 USDT каждую секунду: dx^2/dt = 1 => sigma = 1 USDT/sqrt(s).
    EwmaVolatility v(5.0, 0.0, 0);
    for (int i = 0; i <= 300; ++i) {
        v.update(i * 1000, (i % 2 == 0) ? 100.0 : 101.0);
    }
    EXPECT_NEAR(v.sigma(), 1.0, 1e-9);
    EXPECT_EQ(v.samples(), 300U);
}

TEST(EwmaVolatility, NormalisesByTimeStep) {
    // Шаг ±2 USDT каждые 4 секунды: dx^2/dt = 4/4 = 1.
    EwmaVolatility v(5.0, 0.0, 0);
    for (int i = 0; i <= 300; ++i) {
        v.update(i * 4000, (i % 2 == 0) ? 100.0 : 102.0);
    }
    EXPECT_NEAR(v.sigma(), 1.0, 1e-9);
}

TEST(EwmaVolatility, RespectsMinInterval) {
    EwmaVolatility v(5.0, 0.0, 1000);
    for (int i = 0; i <= 100; ++i) {
        v.update(i * 100, 100.0 + i);
    }
    // 100 мс шаг, сэмпл раз в 1000 мс => 10 приращений.
    EXPECT_EQ(v.samples(), 10U);
}

TEST(EwmaVolatility, IgnoresOutOfOrderAndDuplicateTimestamps) {
    EwmaVolatility v(5.0, 0.0, 0);
    v.update(1000, 100.0);
    v.update(1000, 150.0);
    v.update(500, 10.0);
    EXPECT_EQ(v.samples(), 0U);
    EXPECT_DOUBLE_EQ(v.sigma(), 0.0);
}

TEST(EwmaVolatility, RejectsInvalidParameters) {
    EXPECT_THROW(EwmaVolatility(0.0), std::invalid_argument);
    EXPECT_THROW(EwmaVolatility(1.0, -1.0), std::invalid_argument);
    EXPECT_THROW(EwmaVolatility(1.0, 0.0, -5), std::invalid_argument);
}

}  // namespace
}  // namespace asmm
