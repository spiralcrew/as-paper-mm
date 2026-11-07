#include <gtest/gtest.h>

#include <stdexcept>

#include "asmm/order_book.hpp"

namespace asmm {
namespace {

DepthSnapshot snapshot(std::uint64_t id) {
    DepthSnapshot s;
    s.last_update_id = id;
    s.bids = {{100.00, 1.0}, {99.99, 2.0}, {99.98, 3.0}};
    s.asks = {{100.01, 1.5}, {100.02, 2.5}};
    return s;
}

DepthUpdate update(std::uint64_t first, std::uint64_t last, std::vector<PriceLevel> bids,
                   std::vector<PriceLevel> asks) {
    DepthUpdate u;
    u.first_update_id = first;
    u.final_update_id = last;
    u.bids = std::move(bids);
    u.asks = std::move(asks);
    return u;
}

TEST(OrderBook, AppliesSnapshot) {
    OrderBook book(0.01);
    EXPECT_FALSE(book.synced());
    book.apply_snapshot(snapshot(100));
    EXPECT_TRUE(book.synced());
    EXPECT_DOUBLE_EQ(book.best_bid()->price, 100.00);
    EXPECT_DOUBLE_EQ(book.best_ask()->price, 100.01);
    EXPECT_DOUBLE_EQ(book.qty_at(Side::Buy, 99.99), 2.0);
    EXPECT_DOUBLE_EQ(book.qty_at(Side::Sell, 100.02), 2.5);
    EXPECT_DOUBLE_EQ(book.qty_at(Side::Sell, 100.03), 0.0);
    EXPECT_EQ(book.levels(Side::Buy), 3U);
    EXPECT_EQ(book.last_update_id(), 100U);
}

TEST(OrderBook, RejectsUpdatesWithoutSnapshot) {
    OrderBook book;
    EXPECT_EQ(book.apply_update(update(1, 2, {}, {})), BookUpdateResult::NoSnapshot);
}

TEST(OrderBook, DropsStaleUpdates) {
    OrderBook book;
    book.apply_snapshot(snapshot(100));
    EXPECT_EQ(book.apply_update(update(90, 100, {{100.0, 9.0}}, {})), BookUpdateResult::Stale);
    EXPECT_DOUBLE_EQ(book.qty_at(Side::Buy, 100.0), 1.0);
}

TEST(OrderBook, FirstUpdateMayOverlapSnapshot) {
    // Первое событие после снимка: U <= lastUpdateId+1 <= u.
    OrderBook book;
    book.apply_snapshot(snapshot(100));
    std::vector<LevelChange> changes;
    EXPECT_EQ(book.apply_update(update(95, 105, {{100.00, 0.0}, {99.97, 4.0}}, {{100.01, 0.7}}),
                                &changes),
              BookUpdateResult::Applied);
    EXPECT_EQ(book.last_update_id(), 105U);
    EXPECT_DOUBLE_EQ(book.best_bid()->price, 99.99);
    EXPECT_DOUBLE_EQ(book.qty_at(Side::Buy, 99.97), 4.0);
    EXPECT_DOUBLE_EQ(book.best_ask()->qty, 0.7);
    ASSERT_EQ(changes.size(), 3U);
    EXPECT_EQ(changes[0].side, Side::Buy);
    EXPECT_DOUBLE_EQ(changes[0].qty, 0.0);
    EXPECT_EQ(changes[2].side, Side::Sell);
}

TEST(OrderBook, DetectsGapAndRequiresNewSnapshot) {
    OrderBook book;
    book.apply_snapshot(snapshot(100));
    ASSERT_EQ(book.apply_update(update(101, 110, {}, {})), BookUpdateResult::Applied);
    EXPECT_EQ(book.apply_update(update(112, 120, {}, {})), BookUpdateResult::Gap);
    EXPECT_FALSE(book.synced());
    EXPECT_EQ(book.apply_update(update(121, 130, {}, {})), BookUpdateResult::NoSnapshot);
    book.apply_snapshot(snapshot(125));
    EXPECT_EQ(book.apply_update(update(121, 130, {}, {})), BookUpdateResult::Applied);
}

TEST(OrderBook, TopAndTopLevels) {
    OrderBook book;
    book.apply_snapshot(snapshot(7));
    const auto top = book.top();
    ASSERT_TRUE(top.has_value());
    EXPECT_DOUBLE_EQ(top->mid(), 100.005);
    EXPECT_EQ(top->update_id, 7U);
    const auto bids = book.top_levels(Side::Buy, 2);
    ASSERT_EQ(bids.size(), 2U);
    EXPECT_DOUBLE_EQ(bids[0].price, 100.00);
    EXPECT_DOUBLE_EQ(bids[1].price, 99.99);
    EXPECT_EQ(book.top_levels(Side::Sell, 10).size(), 2U);
}

TEST(OrderBook, PricesAreMatchedOnTickGrid) {
    // 80467.81 из строки и 80467.80 + 0.01 в double различаются, но это один тик.
    OrderBook book(0.01);
    DepthSnapshot s;
    s.last_update_id = 1;
    s.bids = {{80467.81, 2.0}};
    s.asks = {{80467.82, 1.0}};
    book.apply_snapshot(s);
    EXPECT_DOUBLE_EQ(book.qty_at(Side::Buy, 80467.80 + 0.01), 2.0);
}

TEST(OrderBook, RejectsInvalidTick) {
    EXPECT_THROW(OrderBook(0.0), std::invalid_argument);
}

}  // namespace
}  // namespace asmm
