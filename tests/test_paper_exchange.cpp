#include <gtest/gtest.h>

#include <map>
#include <utility>
#include <vector>

#include "asmm/paper_exchange.hpp"

namespace asmm {
namespace {

Trade sell_aggressor(double price, double qty, std::int64_t ts = 0) {
    return Trade{price, qty, true, 1, ts};
}
Trade buy_aggressor(double price, double qty, std::int64_t ts = 0) {
    return Trade{price, qty, false, 2, ts};
}

/// Простая книга для источника объёма уровней.
struct FakeBook {
    std::map<std::pair<int, double>, double> levels;
    double qty(Side side, double price) const {
        const auto it = levels.find({side == Side::Buy ? 0 : 1, price});
        return it == levels.end() ? 0.0 : it->second;
    }
    void set(Side side, double price, double qty) {
        levels[{side == Side::Buy ? 0 : 1, price}] = qty;
    }
};

PaperExchange queue_exchange(const FakeBook& book, CancelModel cancel = CancelModel::Proportional,
                             std::int64_t latency_ms = 0) {
    ExchangeConfig cfg;
    cfg.fill_model = FillModel::Queue;
    cfg.cancel_model = cancel;
    cfg.latency_ms = latency_ms;
    PaperExchange ex(cfg);
    ex.set_level_source([&book](Side s, double p) { return book.qty(s, p); });
    return ex;
}

// --- Граничные модели Touch / Through -------------------------------------

TEST(PaperExchange, SellAggressorAtOurBidFillsOnTouch) {
    PaperExchange ex(FillModel::Touch, 0.0);
    ex.set_quotes(99.0, 101.0, 1.0);
    const auto fills = ex.on_trade(sell_aggressor(99.0, 5.0));
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_EQ(fills[0].side, Side::Buy);
    EXPECT_DOUBLE_EQ(fills[0].price, 99.0);
    EXPECT_DOUBLE_EQ(fills[0].qty, 1.0);
    EXPECT_DOUBLE_EQ(ex.position(), 1.0);
    EXPECT_DOUBLE_EQ(ex.cash(), -99.0);
    EXPECT_FALSE(ex.bid().active());
}

TEST(PaperExchange, BuyAggressorNeverFillsOurBid) {
    PaperExchange ex(FillModel::Touch, 0.0);
    ex.set_quotes(99.0, 101.0, 1.0);
    EXPECT_TRUE(ex.on_trade(buy_aggressor(98.0, 5.0)).empty());
    EXPECT_DOUBLE_EQ(ex.position(), 0.0);
}

TEST(PaperExchange, TradeAboveBidDoesNotFill) {
    PaperExchange ex(FillModel::Touch, 0.0);
    ex.set_quotes(99.0, 101.0, 1.0);
    EXPECT_TRUE(ex.on_trade(sell_aggressor(99.5, 5.0)).empty());
}

TEST(PaperExchange, ThroughModelNeedsPriceImprovement) {
    PaperExchange ex(FillModel::Through, 0.0);
    ex.set_quotes(99.0, 101.0, 1.0);
    EXPECT_TRUE(ex.on_trade(sell_aggressor(99.0, 5.0)).empty());
    EXPECT_TRUE(ex.on_trade(buy_aggressor(101.0, 5.0)).empty());
    EXPECT_EQ(ex.on_trade(sell_aggressor(98.99, 5.0)).size(), 1U);
    EXPECT_EQ(ex.on_trade(buy_aggressor(101.01, 5.0)).size(), 1U);
    EXPECT_DOUBLE_EQ(ex.position(), 0.0);
}

TEST(PaperExchange, PartialFillKeepsRemainder) {
    PaperExchange ex;
    ex.set_quotes(99.0, 101.0, 1.0);
    const auto fills = ex.on_trade(buy_aggressor(101.0, 0.4));
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_DOUBLE_EQ(fills[0].qty, 0.4);
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
    ASSERT_EQ(ex.on_trade(sell_aggressor(100.0, 1.0)).size(), 1U);
    ASSERT_EQ(ex.on_trade(buy_aggressor(101.0, 1.0)).size(), 1U);
    EXPECT_DOUBLE_EQ(ex.position(), 0.0);
    EXPECT_DOUBLE_EQ(ex.cash(), 1.0);
    EXPECT_DOUBLE_EQ(ex.equity(500.0), 1.0);
    EXPECT_EQ(ex.fill_count(), 2U);
    EXPECT_DOUBLE_EQ(ex.volume_quote(), 201.0);
}

TEST(PaperExchange, ChargesMakerFee) {
    PaperExchange ex(FillModel::Touch, 0.001);
    ex.set_quotes(100.0, 101.0, 1.0);
    const auto fills = ex.on_trade(sell_aggressor(100.0, 1.0));
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_NEAR(fills[0].fee, 0.1, 1e-12);
    EXPECT_NEAR(ex.cash(), -100.1, 1e-12);
    EXPECT_NEAR(ex.fees(), 0.1, 1e-12);
}

TEST(PaperExchange, RemovedSideIsNotFilled) {
    PaperExchange ex;
    ex.set_quotes(std::nullopt, 101.0, 1.0);
    EXPECT_TRUE(ex.on_trade(sell_aggressor(50.0, 1.0)).empty());
    ex.cancel_all();
    EXPECT_TRUE(ex.on_trade(buy_aggressor(200.0, 1.0)).empty());
}

// --- Модель очереди ---------------------------------------------------------

TEST(QueueModel, JoinsBackOfVisibleLevel) {
    FakeBook book;
    book.set(Side::Buy, 100.0, 3.0);
    PaperExchange ex = queue_exchange(book);
    ex.set_quotes(100.0, std::nullopt, 1.0, 0.0);
    EXPECT_DOUBLE_EQ(ex.bid().queue_ahead, 3.0);
    EXPECT_DOUBLE_EQ(ex.bid().level_qty, 3.0);
}

TEST(QueueModel, TradesConsumeQueueBeforeUs) {
    FakeBook book;
    book.set(Side::Buy, 100.0, 3.0);
    PaperExchange ex = queue_exchange(book);
    ex.set_quotes(100.0, std::nullopt, 1.0, 0.0);

    EXPECT_TRUE(ex.on_trade(sell_aggressor(100.0, 2.0)).empty());
    EXPECT_DOUBLE_EQ(ex.bid().queue_ahead, 1.0);

    // 1.5 BTC: 1.0 добирает очередь, 0.5 исполняет нас.
    const auto fills = ex.on_trade(sell_aggressor(100.0, 1.5));
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_DOUBLE_EQ(fills[0].qty, 0.5);
    EXPECT_DOUBLE_EQ(ex.bid().queue_ahead, 0.0);
    EXPECT_DOUBLE_EQ(ex.bid().qty, 0.5);
}

TEST(QueueModel, TradeThroughLevelFillsCompletely) {
    FakeBook book;
    book.set(Side::Buy, 100.0, 50.0);
    PaperExchange ex = queue_exchange(book);
    ex.set_quotes(100.0, std::nullopt, 1.0, 0.0);
    const auto fills = ex.on_trade(sell_aggressor(99.99, 0.01));
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_DOUBLE_EQ(fills[0].qty, 1.0);
}

TEST(QueueModel, ProportionalCancellationsAdvanceQueue) {
    FakeBook book;
    book.set(Side::Sell, 101.0, 4.0);
    PaperExchange ex = queue_exchange(book, CancelModel::Proportional);
    ex.set_quotes(std::nullopt, 101.0, 0.0, 1.0);
    ASSERT_DOUBLE_EQ(ex.ask().queue_ahead, 4.0);
    // Новые заявки встают за нами — очередь перед нами не меняется.
    ex.on_level_update(Side::Sell, 101.0, 6.0);
    EXPECT_DOUBLE_EQ(ex.ask().queue_ahead, 4.0);
    // Уровень 6 → 3: отменили 3, из них доля 4/6 — перед нами.
    ex.on_level_update(Side::Sell, 101.0, 3.0);
    EXPECT_NEAR(ex.ask().queue_ahead, 2.0, 1e-12);
    // Уровень 3 → 1: отменили 2, перед нами доля 2/3 → 2 − 2·(2/3) = 2/3.
    ex.on_level_update(Side::Sell, 101.0, 1.0);
    EXPECT_NEAR(ex.ask().queue_ahead, 2.0 / 3.0, 1e-12);
    // Уровень исчез целиком — мы первые.
    ex.on_level_update(Side::Sell, 101.0, 0.0);
    EXPECT_DOUBLE_EQ(ex.ask().queue_ahead, 0.0);
}

TEST(QueueModel, PessimisticCancellationsOnlyClamp) {
    FakeBook book;
    book.set(Side::Sell, 101.0, 4.0);
    PaperExchange ex = queue_exchange(book, CancelModel::Pessimistic);
    ex.set_quotes(std::nullopt, 101.0, 0.0, 1.0);
    ex.on_level_update(Side::Sell, 101.0, 6.0);
    ex.on_level_update(Side::Sell, 101.0, 4.5);
    EXPECT_DOUBLE_EQ(ex.ask().queue_ahead, 4.0);
    ex.on_level_update(Side::Sell, 101.0, 2.5);
    EXPECT_DOUBLE_EQ(ex.ask().queue_ahead, 2.5);
}

TEST(QueueModel, RequoteAtSamePriceKeepsQueuePosition) {
    FakeBook book;
    book.set(Side::Buy, 100.0, 3.0);
    PaperExchange ex = queue_exchange(book);
    ex.set_quotes(100.0, std::nullopt, 1.0, 0.0);
    ex.on_trade(sell_aggressor(100.0, 2.0));
    ASSERT_DOUBLE_EQ(ex.bid().queue_ahead, 1.0);
    ex.set_quotes(100.0, std::nullopt, 1.0, 0.0);
    EXPECT_DOUBLE_EQ(ex.bid().queue_ahead, 1.0);
    // Переставили на другую цену — снова в хвост очереди нового уровня.
    book.set(Side::Buy, 99.99, 7.0);
    ex.set_quotes(99.99, std::nullopt, 1.0, 0.0);
    EXPECT_DOUBLE_EQ(ex.bid().queue_ahead, 7.0);
}

TEST(QueueModel, FillModelsAreOrderedTouchQueueThrough) {
    // Одни и те же котировки и лента сделок: исполненный объём Touch ≥ Queue ≥ Through.
    FakeBook book;
    book.set(Side::Buy, 100.0, 0.6);
    book.set(Side::Sell, 101.0, 0.3);
    const std::vector<Trade> tape{sell_aggressor(100.0, 0.4), buy_aggressor(101.0, 0.2),
                                  sell_aggressor(100.0, 0.5), buy_aggressor(101.0, 0.4),
                                  sell_aggressor(99.9, 0.01), buy_aggressor(101.2, 0.01)};
    auto filled = [&](FillModel model) {
        ExchangeConfig cfg;
        cfg.fill_model = model;
        PaperExchange ex(cfg);
        ex.set_level_source([&book](Side s, double p) { return book.qty(s, p); });
        ex.set_quotes(100.0, 101.0, 1.0, 1.0, 0);
        double volume = 0.0;
        for (const auto& t : tape) {
            for (const auto& f : ex.on_trade(t)) {
                volume += f.qty;
            }
        }
        return volume;
    };
    const double touch = filled(FillModel::Touch);
    const double queue = filled(FillModel::Queue);
    const double through = filled(FillModel::Through);
    EXPECT_GE(touch, queue);
    EXPECT_GE(queue, through);
    EXPECT_NEAR(touch, 2.0, 1e-12);    // 0.9 + 0.6 по своим ценам, остатки — пробоем
    EXPECT_NEAR(queue, 2.0, 1e-12);    // пробой в конце добирает обе заявки целиком
    EXPECT_NEAR(through, 2.0, 1e-12);  // только пробой, но он исполняет заявки целиком
}

TEST(QueueModel, PartialAtPriceFillsDifferByModel) {
    FakeBook book;
    book.set(Side::Buy, 100.0, 0.6);
    const std::vector<Trade> tape{sell_aggressor(100.0, 0.4), sell_aggressor(100.0, 0.5)};
    auto filled = [&](FillModel model) {
        ExchangeConfig cfg;
        cfg.fill_model = model;
        PaperExchange ex(cfg);
        ex.set_level_source([&book](Side s, double p) { return book.qty(s, p); });
        ex.set_quotes(100.0, std::nullopt, 1.0, 0.0, 0);
        for (const auto& t : tape) {
            ex.on_trade(t);
        }
        return ex.position();
    };
    EXPECT_NEAR(filled(FillModel::Touch), 0.9, 1e-12);
    EXPECT_NEAR(filled(FillModel::Queue), 0.3, 1e-12);  // 0.6 очереди перед нами
    EXPECT_NEAR(filled(FillModel::Through), 0.0, 1e-12);
}

// --- Задержка ---------------------------------------------------------------

TEST(Latency, NewOrderIsNotLiveUntilLatencyPasses) {
    FakeBook book;
    PaperExchange ex = queue_exchange(book, CancelModel::Proportional, 50);
    ex.set_quotes(100.0, std::nullopt, 1.0, 0.0, 1'000);
    EXPECT_FALSE(ex.bid().live);
    EXPECT_TRUE(ex.on_trade(sell_aggressor(99.0, 1.0, 1'040)).empty());
    EXPECT_EQ(ex.on_trade(sell_aggressor(99.0, 1.0, 1'050)).size(), 1U);
}

TEST(Latency, StaleQuoteCanBeHitBeforeCancelArrives) {
    FakeBook book;
    PaperExchange ex = queue_exchange(book, CancelModel::Proportional, 50);
    ex.set_quotes(100.0, std::nullopt, 1.0, 0.0, 0);
    ex.advance_to(50);
    ASSERT_TRUE(ex.bid().live);
    // Переставляем бид ниже в t=100; до t=150 старая заявка ещё в книге.
    ex.set_quotes(99.0, std::nullopt, 1.0, 0.0, 100);
    const auto fills = ex.on_trade(sell_aggressor(99.5, 1.0, 120));
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_DOUBLE_EQ(fills[0].price, 100.0);
    // После t=150 старой заявки нет, новая живая по 99.
    ex.advance_to(150);
    EXPECT_EQ(ex.orders().size(), 1U);
    EXPECT_DOUBLE_EQ(ex.bid().price, 99.0);
    EXPECT_TRUE(ex.bid().live);
}

// --- Рыночные заявки и проскальзывание ---------------------------------------

TEST(MarketOrder, WalksBookAndMeasuresSlippage) {
    ExchangeConfig cfg;
    cfg.taker_fee_rate = 0.001;
    PaperExchange ex(cfg);
    const std::vector<PriceLevel> bids{{100.0, 0.5}, {99.9, 0.3}, {99.5, 1.0}};
    const MarketFill mf = ex.market_order(Side::Sell, 1.0, bids, 100.05, 0);
    // 0.5*100 + 0.3*99.9 + 0.2*99.5 = 99.87
    EXPECT_NEAR(mf.vwap, 99.87, 1e-12);
    EXPECT_EQ(mf.levels, 3U);
    EXPECT_TRUE(mf.complete);
    EXPECT_NEAR(mf.slippage, 0.18, 1e-12);
    EXPECT_NEAR(mf.fee, 0.09987, 1e-12);
    EXPECT_DOUBLE_EQ(ex.position(), -1.0);
    EXPECT_NEAR(ex.cash(), 99.87 - 0.09987, 1e-12);
    EXPECT_NEAR(ex.slippage_cost(), 0.18, 1e-12);
}

TEST(MarketOrder, ReportsExhaustedBook) {
    PaperExchange ex;
    const MarketFill mf = ex.market_order(Side::Buy, 2.0, {{101.0, 0.5}}, 100.5, 0);
    EXPECT_FALSE(mf.complete);
    EXPECT_DOUBLE_EQ(mf.vwap, 101.0);
    EXPECT_DOUBLE_EQ(ex.position(), 2.0);
    const MarketFill none = ex.market_order(Side::Buy, 1.0, {}, 100.5, 0);
    EXPECT_DOUBLE_EQ(none.qty, 0.0);
    EXPECT_FALSE(none.complete);
}

}  // namespace
}  // namespace asmm
