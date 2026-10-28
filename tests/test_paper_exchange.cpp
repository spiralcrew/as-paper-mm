#include <gtest/gtest.h>

#include "asmm/paper_exchange.hpp"

namespace asmm {
namespace {

Trade sell_aggressor(double price, double qty) {
    return Trade{price, qty, true, 1, 0};
}
Trade buy_aggressor(double price, double qty) {
    return Trade{price, qty, false, 2, 0};
}

TEST(PaperExchange, SellAggressorAtOurBidFillsOnTouch) {
    PaperExchange ex(FillModel::Touch);
    ex.set_quotes(99.0, 101.0, 1.0);
    const auto fill = ex.on_trade(sell_aggressor(99.0, 5.0));
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->side, Side::Buy);
    EXPECT_DOUBLE_EQ(fill->price, 99.0);
    EXPECT_DOUBLE_EQ(fill->qty, 1.0);
    EXPECT_DOUBLE_EQ(ex.position(), 1.0);
    EXPECT_DOUBLE_EQ(ex.cash(), -99.0);
    EXPECT_FALSE(ex.bid().active());
}

TEST(PaperExchange, BuyAggressorNeverFillsOurBid) {
    PaperExchange ex(FillModel::Touch);
    ex.set_quotes(99.0, 101.0, 1.0);
    EXPECT_FALSE(ex.on_trade(buy_aggressor(98.0, 5.0)).has_value());
    EXPECT_DOUBLE_EQ(ex.position(), 0.0);
}

TEST(PaperExchange, TradeAboveBidDoesNotFill) {
    PaperExchange ex(FillModel::Touch);
    ex.set_quotes(99.0, 101.0, 1.0);
    EXPECT_FALSE(ex.on_trade(sell_aggressor(99.5, 5.0)).has_value());
}

TEST(PaperExchange, ThroughModelNeedsPriceImprovement) {
    PaperExchange ex(FillModel::Through);
    ex.set_quotes(99.0, 101.0, 1.0);
    EXPECT_FALSE(ex.on_trade(sell_aggressor(99.0, 5.0)).has_value());
    EXPECT_FALSE(ex.on_trade(buy_aggressor(101.0, 5.0)).has_value());
    EXPECT_TRUE(ex.on_trade(sell_aggressor(98.99, 5.0)).has_value());
    EXPECT_TRUE(ex.on_trade(buy_aggressor(101.01, 5.0)).has_value());
    EXPECT_DOUBLE_EQ(ex.position(), 0.0);
}

TEST(PaperExchange, PartialFillKeepsRemainder) {
    PaperExchange ex;
    ex.set_quotes(99.0, 101.0, 1.0);
    const auto fill = ex.on_trade(buy_aggressor(101.0, 0.4));
    ASSERT_TRUE(fill.has_value());
    EXPECT_DOUBLE_EQ(fill->qty, 0.4);
    EXPECT_NEAR(ex.ask().qty, 0.6, 1e-12);
    EXPECT_DOUBLE_EQ(ex.position(), -0.4);

    // Та же цена — остаток не доливается до полного объёма.
    ex.set_quotes(99.0, 101.0, 1.0);
    EXPECT_NEAR(ex.ask().qty, 0.6, 1e-12);
    // Новая цена — заявка переставлена с полным объёмом.
    ex.set_quotes(99.0, 101.5, 1.0);
    EXPECT_DOUBLE_EQ(ex.ask().qty, 1.0);
}

TEST(PaperExchange, RoundTripEarnsSpread) {
    PaperExchange ex;
    ex.set_quotes(100.0, 101.0, 1.0);
    ASSERT_TRUE(ex.on_trade(sell_aggressor(100.0, 1.0)));
    ASSERT_TRUE(ex.on_trade(buy_aggressor(101.0, 1.0)));
    EXPECT_DOUBLE_EQ(ex.position(), 0.0);
    EXPECT_DOUBLE_EQ(ex.cash(), 1.0);
    EXPECT_DOUBLE_EQ(ex.equity(500.0), 1.0);
    EXPECT_EQ(ex.fill_count(), 2U);
    EXPECT_DOUBLE_EQ(ex.volume_quote(), 201.0);
}

TEST(PaperExchange, ChargesMakerFee) {
    PaperExchange ex(FillModel::Touch, 0.001);
    ex.set_quotes(100.0, 101.0, 1.0);
    const auto fill = ex.on_trade(sell_aggressor(100.0, 1.0));
    ASSERT_TRUE(fill.has_value());
    EXPECT_NEAR(fill->fee, 0.1, 1e-12);
    EXPECT_NEAR(ex.cash(), -100.1, 1e-12);
    EXPECT_NEAR(ex.fees(), 0.1, 1e-12);
}

TEST(PaperExchange, RemovedSideIsNotFilled) {
    PaperExchange ex;
    ex.set_quotes(std::nullopt, 101.0, 1.0);
    EXPECT_FALSE(ex.on_trade(sell_aggressor(50.0, 1.0)).has_value());
    ex.cancel_all();
    EXPECT_FALSE(ex.on_trade(buy_aggressor(200.0, 1.0)).has_value());
}

}  // namespace
}  // namespace asmm
