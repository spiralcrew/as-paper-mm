#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>

#include "asmm/binance_parser.hpp"
#include "asmm/engine.hpp"

namespace asmm {
namespace {

EngineConfig test_config() {
    EngineConfig c;
    c.as = AsParams{0.001, 2.0, 30.0};
    c.order_qty = 0.001;
    c.max_inventory_lots = 2.0;
    c.initial_sigma = 2.0;
    return c;
}

BookTop book(double bid, double ask, std::int64_t ts) {
    return BookTop{bid, 1.0, ask, 1.0, 1, ts};
}

TEST(MarketMakerEngine, QuotesBothSidesAroundMid) {
    MarketMakerEngine e(test_config());
    e.on_book(book(100.00, 100.02, 1'000));
    const EngineStats s = e.stats();
    EXPECT_TRUE(s.bid_active);
    EXPECT_TRUE(s.ask_active);
    EXPECT_DOUBLE_EQ(s.reservation_price, 100.01);
    // spread = 0.001*4*30 + 2000*ln(1+0.0005) ≈ 1.11975 => bid 99.45, ask 100.57
    EXPECT_NEAR(s.bid, 99.45, 1e-9);
    EXPECT_NEAR(s.ask, 100.57, 1e-9);
    EXPECT_LT(s.bid, s.ask);
}

TEST(MarketMakerEngine, FillUpdatesInventoryAndSkewsQuotesDown) {
    MarketMakerEngine e(test_config());
    e.on_book(book(100.00, 100.02, 1'000));
    const double r0 = e.stats().reservation_price;
    EXPECT_EQ(e.on_trade(Trade{99.40, 1.0, true, 1, 1'100}), 1U);
    const EngineStats s = e.stats();
    EXPECT_NEAR(s.inventory_btc, 0.001, 1e-12);
    EXPECT_NEAR(s.inventory_lots, 1.0, 1e-9);
    EXPECT_LT(s.reservation_price, r0);
    EXPECT_EQ(s.fills, 1U);
}

TEST(MarketMakerEngine, InventoryLimitRemovesBid) {
    MarketMakerEngine e(test_config());
    e.on_book(book(100.00, 100.02, 1'000));
    ASSERT_EQ(e.on_trade(Trade{90.0, 1.0, true, 1, 1'100}), 1U);
    ASSERT_EQ(e.on_trade(Trade{90.0, 1.0, true, 2, 1'200}), 1U);
    const EngineStats s = e.stats();
    EXPECT_NEAR(s.inventory_lots, 2.0, 1e-9);
    EXPECT_FALSE(s.bid_active);
    EXPECT_TRUE(s.ask_active);
    EXPECT_EQ(e.on_trade(Trade{80.0, 1.0, true, 3, 1'300}), 0U);
}

TEST(MarketMakerEngine, TauCountsDownAndWrapsAround) {
    MarketMakerEngine e(test_config());
    e.on_book(book(100.00, 100.02, 10'000));
    EXPECT_DOUBLE_EQ(e.tau_at(10'000), 30.0);
    EXPECT_DOUBLE_EQ(e.tau_at(20'000), 20.0);
    EXPECT_DOUBLE_EQ(e.tau_at(40'000), 30.0);
    EXPECT_DOUBLE_EQ(e.tau_at(45'000), 25.0);
}

TEST(MarketMakerEngine, IgnoresInvalidBook) {
    MarketMakerEngine e(test_config());
    e.on_book(book(100.02, 100.00, 1'000));
    EXPECT_EQ(e.stats().book_updates, 0U);
    EXPECT_FALSE(e.stats().bid_active);
}

TEST(MarketMakerEngine, RejectsInvalidConfig) {
    EngineConfig c = test_config();
    c.order_qty = 0.0;
    EXPECT_THROW(MarketMakerEngine{c}, std::invalid_argument);
}

TEST(MarketMakerEngine, ReplaysRecordedBinanceSample) {
    std::ifstream in(ASMM_SAMPLE_DATA);
    ASSERT_TRUE(in.is_open()) << "missing sample: " << ASMM_SAMPLE_DATA;
    MarketMakerEngine e(EngineConfig{});
    std::string line;
    std::size_t parsed = 0;
    while (std::getline(in, line)) {
        if (const auto msg = parse_binance_message(line)) {
            e.on_message(*msg);
            ++parsed;
        }
    }
    const EngineStats s = e.stats();
    EXPECT_GT(parsed, 100U);
    EXPECT_GT(s.book_updates, 100U);
    EXPECT_GT(s.trades, 0U);
    EXPECT_TRUE(std::isfinite(s.equity));
    EXPECT_LE(std::abs(s.inventory_lots), EngineConfig{}.max_inventory_lots + 1e-9);
    // Тождество учёта: equity = cash + position * mid.
    EXPECT_NEAR(s.equity, s.cash + s.inventory_btc * s.mid, 1e-6);
}

}  // namespace
}  // namespace asmm
