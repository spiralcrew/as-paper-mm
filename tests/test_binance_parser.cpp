#include <gtest/gtest.h>

#include "asmm/binance_parser.hpp"

namespace asmm {
namespace {

TEST(BinanceParser, ParsesCombinedBookTickerWithRecvTime) {
    const auto msg = parse_binance_message(
        R"({"recv_ms":1761650000123,"stream":"btcusdt@bookTicker","data":{"u":101998352196,)"
        R"("s":"BTCUSDT","b":"80467.81000000","B":"4.39220000","a":"80467.82000000","A":"1.18021000"}})");
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->type, MessageType::BookTicker);
    EXPECT_DOUBLE_EQ(msg->book.bid_price, 80467.81);
    EXPECT_DOUBLE_EQ(msg->book.bid_qty, 4.3922);
    EXPECT_DOUBLE_EQ(msg->book.ask_price, 80467.82);
    EXPECT_DOUBLE_EQ(msg->book.ask_qty, 1.18021);
    EXPECT_EQ(msg->book.update_id, 101998352196U);
    EXPECT_EQ(msg->book.ts_ms, 1761650000123);
}

TEST(BinanceParser, ParsesCombinedTrade) {
    const auto msg = parse_binance_message(
        R"({"recv_ms":1761650000200,"stream":"btcusdt@trade","data":{"e":"trade","E":1761650000150,)"
        R"("s":"BTCUSDT","t":5383210123,"p":"80467.82000000","q":"0.00150000","T":1761650000149,)"
        R"("m":false,"M":true}})");
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->type, MessageType::Trade);
    EXPECT_DOUBLE_EQ(msg->trade.price, 80467.82);
    EXPECT_DOUBLE_EQ(msg->trade.qty, 0.0015);
    EXPECT_FALSE(msg->trade.buyer_is_maker);
    EXPECT_EQ(msg->trade.trade_id, 5383210123U);
    EXPECT_EQ(msg->trade.ts_ms, 1761650000200);
}

TEST(BinanceParser, FallsBackToTradeTimeWithoutRecvTime) {
    const auto msg = parse_binance_message(
        R"({"e":"trade","E":1761650000150,"s":"BTCUSDT","t":1,"p":"100.5","q":"2",)"
        R"("T":1761650000149,"m":true,"M":true})");
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->type, MessageType::Trade);
    EXPECT_TRUE(msg->trade.buyer_is_maker);
    EXPECT_EQ(msg->trade.ts_ms, 1761650000149);
}

TEST(BinanceParser, ParsesRawBookTicker) {
    const auto msg = parse_binance_message(
        R"({"u":400900217,"s":"BNBUSDT","b":"25.35190000","B":"31.21000000","a":"25.36520000","A":"40.66000000"})");
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->type, MessageType::BookTicker);
    EXPECT_DOUBLE_EQ(msg->book.bid_price, 25.3519);
    EXPECT_DOUBLE_EQ(msg->book.ask_price, 25.3652);
    EXPECT_EQ(msg->book.ts_ms, 0);
}

TEST(BinanceParser, ParsesAggTrade) {
    const auto msg = parse_binance_message(
        R"({"stream":"btcusdt@aggTrade","data":{"e":"aggTrade","E":1,"s":"BTCUSDT","a":42,)"
        R"("p":"100.0","q":"0.5","f":1,"l":2,"T":7,"m":true,"M":true}})");
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->type, MessageType::Trade);
    EXPECT_EQ(msg->trade.trade_id, 42U);
    EXPECT_EQ(msg->trade.ts_ms, 7);
}

TEST(BinanceParser, RejectsMalformedAndUnknownMessages) {
    EXPECT_FALSE(parse_binance_message("").has_value());
    EXPECT_FALSE(parse_binance_message("not json").has_value());
    EXPECT_FALSE(parse_binance_message(R"({"result":null,"id":1})").has_value());
    EXPECT_FALSE(parse_binance_message(R"({"stream":"btcusdt@depth","data":{"e":"depthUpdate"}})")
                     .has_value());
    EXPECT_FALSE(
        parse_binance_message(
            R"({"stream":"btcusdt@bookTicker","data":{"b":"abc","B":"1","a":"2","A":"1"}})")
            .has_value());
    EXPECT_FALSE(parse_binance_message(R"({"e":"trade","p":"1","q":"1","m":"maybe"})").has_value());
}

TEST(BinanceParser, ParsesCombinedDepthUpdate) {
    const auto msg = parse_binance_message(
        R"({"recv_ms":1794077812300,"stream":"btcusdt@depth@100ms","data":{"e":"depthUpdate",)"
        R"("E":1794077812250,"s":"BTCUSDT","U":102457769541,"u":102457769560,)"
        R"("b":[["75824.73000000","2.70000000"],["75824.50000000","0.00000000"]],)"
        R"("a":[["75824.74000000","4.10000000"]]}})");
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->type, MessageType::DepthUpdate);
    EXPECT_EQ(msg->depth.first_update_id, 102457769541U);
    EXPECT_EQ(msg->depth.final_update_id, 102457769560U);
    ASSERT_EQ(msg->depth.bids.size(), 2U);
    EXPECT_DOUBLE_EQ(msg->depth.bids[0].price, 75824.73);
    EXPECT_DOUBLE_EQ(msg->depth.bids[0].qty, 2.7);
    EXPECT_DOUBLE_EQ(msg->depth.bids[1].qty, 0.0);
    ASSERT_EQ(msg->depth.asks.size(), 1U);
    EXPECT_DOUBLE_EQ(msg->depth.asks[0].qty, 4.1);
    EXPECT_EQ(msg->depth.ts_ms, 1794077812300);
}

TEST(BinanceParser, ParsesRawDepthUpdateWithEmptySide) {
    const auto msg = parse_binance_message(
        R"({"e":"depthUpdate","E":123,"s":"BTCUSDT","U":5,"u":7,"b":[],"a":[["1.5","2"]]})");
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->type, MessageType::DepthUpdate);
    EXPECT_TRUE(msg->depth.bids.empty());
    EXPECT_EQ(msg->depth.asks.size(), 1U);
    EXPECT_EQ(msg->depth.ts_ms, 123);
}

TEST(BinanceParser, ParsesDepthSnapshot) {
    const auto msg = parse_binance_message(
        R"({"recv_ms":1794077812216,"stream":"btcusdt@depthSnapshot","data":{"lastUpdateId":102457769540,)"
        R"("bids":[["75824.73000000","2.69817000"],["75824.72000000","0.00042000"]],)"
        R"("asks":[["75824.74000000","4.29686000"]]}})");
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->type, MessageType::DepthSnapshot);
    EXPECT_EQ(msg->snapshot.last_update_id, 102457769540U);
    ASSERT_EQ(msg->snapshot.bids.size(), 2U);
    EXPECT_DOUBLE_EQ(msg->snapshot.bids[1].price, 75824.72);
    EXPECT_EQ(msg->snapshot.asks.size(), 1U);
    EXPECT_EQ(msg->snapshot.ts_ms, 1794077812216);

    const auto raw = parse_binance_message(R"({"lastUpdateId":1,"bids":[],"asks":[]})");
    ASSERT_TRUE(raw.has_value());
    EXPECT_EQ(raw->type, MessageType::DepthSnapshot);
}

TEST(BinanceParser, RejectsMalformedDepth) {
    // Обрезанный массив уровней, нечисловая цена, U > u, partial depth стрим.
    EXPECT_FALSE(parse_binance_message(R"({"e":"depthUpdate","U":1,"u":2,"b":[["1","2"],"a":[]})")
                     .has_value());
    EXPECT_FALSE(parse_binance_message(R"({"e":"depthUpdate","U":1,"u":2,"b":[["x","2"]],"a":[]})")
                     .has_value());
    EXPECT_FALSE(
        parse_binance_message(R"({"e":"depthUpdate","U":3,"u":2,"b":[],"a":[]})").has_value());
    EXPECT_FALSE(
        parse_binance_message(
            R"({"stream":"btcusdt@depth20@100ms","data":{"lastUpdateId":1,"bids":[],"asks":[]}})")
            .has_value());
}

}  // namespace
}  // namespace asmm
