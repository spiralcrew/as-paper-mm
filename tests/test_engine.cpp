#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

std::vector<ParsedMessage> load_sample() {
    std::ifstream in(ASMM_SAMPLE_DATA);
    std::vector<ParsedMessage> out;
    std::string line;
    while (std::getline(in, line)) {
        if (auto msg = parse_binance_message(line)) {
            out.push_back(std::move(*msg));
        }
    }
    return out;
}

TEST(MarketMakerEngine, QueuePositionDelaysFillAtTouch) {
    EngineConfig c = test_config();
    c.as.gamma = 0.0001;
    c.as.k = 200.0;  // узкий спред: котировки встают на лучшие цены
    c.initial_sigma = 0.0;
    MarketMakerEngine e(c);
    e.on_book(BookTop{100.00, 2.0, 100.02, 3.0, 1, 1'000});
    const EngineStats s0 = e.stats();
    ASSERT_NEAR(s0.bid, 100.00, 1e-9);
    ASSERT_NEAR(s0.ask, 100.02, 1e-9);
    EXPECT_DOUBLE_EQ(s0.bid_queue, 2.0);
    EXPECT_DOUBLE_EQ(s0.ask_queue, 3.0);
    // Продажа 1.5 BTC по 100.00 съедает часть очереди, нас не исполняет.
    EXPECT_EQ(e.on_trade(Trade{100.00, 1.5, true, 1, 1'100}), 0U);
    EXPECT_NEAR(e.stats().bid_queue, 0.5, 1e-12);
    // Ещё 0.5005: 0.5 — очередь, 0.0005 — наш частичный fill.
    EXPECT_EQ(e.on_trade(Trade{100.00, 0.5005, true, 2, 1'200}), 1U);
    EXPECT_NEAR(e.stats().inventory_btc, 0.0005, 1e-12);
}

TEST(MarketMakerEngine, LiquidationWalksBookWithSlippage) {
    EngineConfig c = test_config();
    MarketMakerEngine e(c);
    DepthSnapshot snap;
    snap.last_update_id = 10;
    snap.bids = {{100.00, 0.0005}, {99.90, 0.0005}, {99.50, 1.0}};
    snap.asks = {{100.02, 1.0}};
    snap.ts_ms = 1'000;
    e.on_depth_snapshot(snap);
    e.on_book(BookTop{100.00, 0.0005, 100.02, 1.0, 10, 1'000});
    ASSERT_EQ(e.on_trade(Trade{90.0, 1.0, true, 1, 1'100}), 1U);  // купили 1 лот по биду
    ASSERT_NEAR(e.stats().inventory_btc, 0.001, 1e-12);
    const auto mf = e.liquidate();
    ASSERT_TRUE(mf.has_value());
    EXPECT_EQ(mf->side, Side::Sell);
    EXPECT_NEAR(mf->vwap, 99.95, 1e-9);  // 0.0005 по 100.00 + 0.0005 по 99.90
    EXPECT_EQ(mf->levels, 2U);
    EXPECT_NEAR(mf->slippage, (100.01 - 99.95) * 0.001, 1e-12);
    const EngineStats s = e.stats();
    EXPECT_NEAR(s.inventory_btc, 0.0, 1e-12);
    EXPECT_EQ(s.taker_orders, 1U);
}

TEST(MarketMakerEngine, LatencyDelaysQuotes) {
    EngineConfig c = test_config();
    c.exchange.latency_ms = 100;
    MarketMakerEngine e(c);
    e.on_book(book(100.00, 100.02, 1'000));
    EXPECT_FALSE(e.exchange().bid().live);
    EXPECT_EQ(e.on_trade(Trade{90.0, 1.0, true, 1, 1'050}), 0U);
    EXPECT_EQ(e.on_trade(Trade{90.0, 1.0, true, 2, 1'100}), 1U);
}

TEST(MarketMakerEngine, ReplaysRecordedBinanceSample) {
    const auto messages = load_sample();
    ASSERT_GT(messages.size(), 1000U) << "missing sample: " << ASMM_SAMPLE_DATA;
    MarketMakerEngine e(EngineConfig{});
    for (const auto& m : messages) {
        e.on_message(m);
    }
    e.liquidate();
    const EngineStats s = e.stats();
    EXPECT_GT(s.book_updates, 100U);
    EXPECT_GT(s.trades, 100U);
    EXPECT_GE(s.depth_snapshots, 1U);
    EXPECT_GT(s.depth_updates, 100U);
    EXPECT_EQ(s.depth_gaps, 0U);
    EXPECT_TRUE(s.book_synced);
    EXPECT_TRUE(s.k_calibrated);
    EXPECT_GE(s.k, EngineConfig{}.k_min);
    EXPECT_LE(s.k, EngineConfig{}.k_max);
    EXPECT_TRUE(std::isfinite(s.equity));
    EXPECT_LE(s.max_abs_inventory_lots, EngineConfig{}.max_inventory_lots + 1e-9);
    EXPECT_NEAR(s.inventory_btc, 0.0, 1e-12);
    // Тождество учёта: equity = cash + position * mid.
    EXPECT_NEAR(s.equity, s.cash + s.inventory_btc * s.mid, 1e-6);
}

TEST(OrderBookOnRealData, MatchesBookTickerAtSameUpdateId) {
    // bookTicker.u и depthUpdate.u — одна последовательность id стакана Binance. bookTicker
    // приходит в реальном времени, а depth@100ms агрегирует обновления за 100 мс, поэтому
    // тикер с id = X приходит раньше пакета, оканчивающегося на X. После применения пакета
    // ищем уже полученный тикер с тем же id: лучшие цены и объёмы должны совпасть точно.
    const auto messages = load_sample();
    OrderBook book(0.01);
    std::map<std::uint64_t, BookTop> tickers;
    std::size_t compared = 0;
    std::size_t mismatched = 0;
    std::size_t gaps = 0;
    for (const auto& m : messages) {
        if (m.type == MessageType::BookTicker) {
            tickers[m.book.update_id] = m.book;
        } else if (m.type == MessageType::DepthSnapshot) {
            book.apply_snapshot(m.snapshot);
        } else if (m.type == MessageType::DepthUpdate) {
            const auto result = book.apply_update(m.depth);
            gaps += result == BookUpdateResult::Gap ? 1U : 0U;
            if (result != BookUpdateResult::Applied) {
                continue;
            }
            const auto it = tickers.find(book.last_update_id());
            if (it != tickers.end()) {
                ++compared;
                const auto top = book.top();
                const BookTop& t = it->second;
                if (!top || std::abs(top->bid_price - t.bid_price) > 1e-9 ||
                    std::abs(top->ask_price - t.ask_price) > 1e-9 ||
                    std::abs(top->bid_qty - t.bid_qty) > 1e-9 ||
                    std::abs(top->ask_qty - t.ask_qty) > 1e-9) {
                    ++mismatched;
                }
            }
            tickers.erase(tickers.begin(), tickers.upper_bound(book.last_update_id()));
        }
    }
    EXPECT_EQ(gaps, 0U);
    EXPECT_GT(compared, 50U);
    EXPECT_EQ(mismatched, 0U);
}

}  // namespace
}  // namespace asmm
