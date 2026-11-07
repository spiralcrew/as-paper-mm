#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <stdexcept>

#include "asmm/intensity.hpp"

namespace asmm {
namespace {

/**
 * Глубина D, до которой дошли агрессоры за интервал Δ, при пуассоновском исполнении
 * с λ(δ) = A e^{-kδ}: P(D >= δ) = 1 − exp(−A e^{-kδ} Δ). Обратное преобразование:
 * D = ln(AΔ / E) / k, где E = −ln(U) ~ Exp(1).
 */
double sample_depth(std::mt19937_64& rng, double A, double k, double dt) {
    const double u = (static_cast<double>(rng() >> 11) + 0.5) * 0x1.0p-53;
    return std::log(A * dt / -std::log(u)) / k;
}

TEST(IntensityEstimator, RecoversKnownParameters) {
    IntensityConfig cfg;
    cfg.window = 20'000;
    IntensityEstimator est(cfg);
    std::mt19937_64 rng(1);
    const double A = 3.0;
    const double k = 1.5;
    for (int i = 0; i < 40'000; ++i) {
        est.add_sample(sample_depth(rng, A, k, cfg.bucket_sec));
    }
    const IntensityFit& fit = est.refit();
    ASSERT_TRUE(fit.valid);
    EXPECT_NEAR(fit.k, k, 0.05 * k);
    EXPECT_NEAR(fit.A, A, 0.10 * A);
    EXPECT_GT(fit.r2, 0.99);
}

TEST(IntensityEstimator, NeedsEnoughSamples) {
    IntensityEstimator est;
    for (int i = 0; i < 50; ++i) {
        est.add_sample(1.0);
    }
    EXPECT_FALSE(est.refit().valid);
}

TEST(IntensityEstimator, MeasuresDepthFromBucketStartMid) {
    IntensityConfig cfg;
    cfg.min_samples = 2;
    cfg.min_hits = 1;
    IntensityEstimator est(cfg);
    est.on_mid(0, 100.0);
    est.on_trade(Trade{100.7, 1.0, false, 1, 100});  // покупатель дошёл до +0.7
    est.on_trade(Trade{99.6, 1.0, true, 2, 200});    // продавец дошёл до −0.4
    est.on_trade(Trade{100.2, 1.0, false, 3, 300});
    est.on_mid(1'000, 100.5);  // интервал закрыт
    EXPECT_EQ(est.samples(), 2U);
}

TEST(IntensityEstimator, EndToEndFromSyntheticTape) {
    // Mid стоит на 100; в каждом интервале сделки доходят до сгенерированной глубины.
    IntensityConfig cfg;
    cfg.window = 5'000;
    IntensityEstimator est(cfg);
    std::mt19937_64 rng(3);
    std::int64_t t = 0;
    for (int bucket = 0; bucket < 5'000; ++bucket) {
        est.on_mid(t, 100.0);
        const double ask_depth = sample_depth(rng, 2.0, 2.0, 1.0);
        const double bid_depth = sample_depth(rng, 2.0, 2.0, 1.0);
        est.on_trade(Trade{100.0 + ask_depth, 0.01, false, 1, t + 300});
        est.on_trade(Trade{100.0 - bid_depth, 0.01, true, 2, t + 600});
        t += 1'000;
    }
    est.on_mid(t, 100.0);
    ASSERT_TRUE(est.fit().valid);
    EXPECT_NEAR(est.fit().k, 2.0, 0.15);
    EXPECT_NEAR(est.fit().A, 2.0, 0.3);
}

TEST(IntensityEstimator, RejectsInvalidConfig) {
    IntensityConfig cfg;
    cfg.bucket_sec = 0.0;
    EXPECT_THROW(IntensityEstimator{cfg}, std::invalid_argument);
}

}  // namespace
}  // namespace asmm
