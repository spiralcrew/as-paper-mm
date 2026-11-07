/**
 * @file bench_main.cpp
 * @brief Бенчмарки горячего пути: формулы AS, парсинг Binance, книга, очередь, движок, эксперимент.
 */
#include <benchmark/benchmark.h>

#include <fstream>
#include <string>
#include <vector>

#include "asmm/as_simulation.hpp"
#include "asmm/avellaneda_stoikov.hpp"
#include "asmm/binance_parser.hpp"
#include "asmm/engine.hpp"
#include "asmm/order_book.hpp"
#include "asmm/paper_exchange.hpp"

namespace {

constexpr const char* kBookLine =
    R"({"recv_ms":1761650000123,"stream":"btcusdt@bookTicker","data":{"u":101998352196,)"
    R"("s":"BTCUSDT","b":"80467.81000000","B":"4.39220000","a":"80467.82000000","A":"1.18021000"}})";

constexpr const char* kTradeLine =
    R"({"recv_ms":1761650000200,"stream":"btcusdt@trade","data":{"e":"trade","E":1761650000150,)"
    R"("s":"BTCUSDT","t":5383210123,"p":"80467.82000000","q":"0.00150000","T":1761650000149,)"
    R"("m":false,"M":true}})";

/// Записанный сэмпл Binance, разобранный заранее (чтобы мерить движок без I/O и парсинга).
const std::vector<asmm::ParsedMessage>& sample_messages() {
    static const std::vector<asmm::ParsedMessage> messages = [] {
        std::vector<asmm::ParsedMessage> out;
        std::ifstream in(ASMM_SAMPLE_DATA);
        std::string line;
        while (std::getline(in, line)) {
            if (auto msg = asmm::parse_binance_message(line)) {
                out.push_back(*msg);
            }
        }
        return out;
    }();
    return messages;
}

void BM_ComputeQuotes(benchmark::State& state) {
    const asmm::AsParams params;
    double inventory = -3.0;
    for (auto _ : state) {
        asmm::Quotes q = asmm::compute_quotes(80467.815, inventory, 2.5, 17.0, params);
        benchmark::DoNotOptimize(q);
        inventory = inventory >= 3.0 ? -3.0 : inventory + 0.5;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ComputeQuotes);

void BM_ComputeAndSnapQuotes(benchmark::State& state) {
    const asmm::AsParams params;
    const asmm::BookTop book{80467.81, 1.0, 80467.82, 1.0, 1, 0};
    for (auto _ : state) {
        const asmm::Quotes raw = asmm::compute_quotes(book.mid(), 1.0, 2.5, 17.0, params);
        benchmark::DoNotOptimize(asmm::snap_to_book(raw, book, 0.01));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ComputeAndSnapQuotes);

void BM_ParseBookTicker(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(asmm::parse_binance_message(kBookLine));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseBookTicker);

void BM_ParseTrade(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(asmm::parse_binance_message(kTradeLine));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseTrade);

void BM_EngineOnBook(benchmark::State& state) {
    asmm::MarketMakerEngine engine(asmm::EngineConfig{});
    asmm::BookTop book{80467.81, 1.0, 80467.82, 1.0, 1, 1'761'650'000'000};
    for (auto _ : state) {
        book.ts_ms += 10;
        engine.on_book(book);
    }
    benchmark::DoNotOptimize(engine.stats());
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EngineOnBook);

/// Полный реплей записанного сэмпла BTCUSDT через движок (без парсинга).
void BM_EngineReplaySample(benchmark::State& state) {
    const auto& messages = sample_messages();
    if (messages.empty()) {
        state.SkipWithError("sample data not found");
        return;
    }
    for (auto _ : state) {
        asmm::MarketMakerEngine engine(asmm::EngineConfig{});
        for (const auto& m : messages) {
            engine.on_message(m);
        }
        benchmark::DoNotOptimize(engine.stats());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(messages.size()));
    state.counters["messages"] = static_cast<double>(messages.size());
}
BENCHMARK(BM_EngineReplaySample)->Unit(benchmark::kMillisecond);

constexpr const char* kDepthLine =
    R"({"recv_ms":1794077812300,"stream":"btcusdt@depth@100ms","data":{"e":"depthUpdate",)"
    R"("E":1794077812250,"s":"BTCUSDT","U":102457769541,"u":102457769560,)"
    R"("b":[["75824.73","2.7"],["75824.50","0"],["75824.10","0.5"],["75823.00","1.2"]],)"
    R"("a":[["75824.74","4.1"],["75825.00","0.3"],["75826.20","0"]]}})";

void BM_ParseDepthUpdate(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(asmm::parse_binance_message(kDepthLine));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseDepthUpdate);

/// Применение обновления к книге из 1000 уровней на сторону.
void BM_OrderBookApplyUpdate(benchmark::State& state) {
    asmm::OrderBook book(0.01);
    asmm::DepthSnapshot snap;
    snap.last_update_id = 1;
    for (int i = 0; i < 1000; ++i) {
        snap.bids.push_back({75000.0 - 0.01 * i, 0.1 + 0.001 * i});
        snap.asks.push_back({75000.01 + 0.01 * i, 0.1 + 0.001 * i});
    }
    book.apply_snapshot(snap);
    asmm::DepthUpdate upd;
    upd.bids = {{75000.0, 1.0}, {74999.5, 0.0}, {74999.0, 2.0}, {74998.0, 0.7}};
    upd.asks = {{75000.01, 1.1}, {75000.5, 0.0}, {75001.0, 0.4}};
    std::uint64_t id = 1;
    std::vector<asmm::LevelChange> changes;
    for (auto _ : state) {
        upd.first_update_id = id + 1;
        upd.final_update_id = id + 10;
        id += 10;
        changes.clear();
        benchmark::DoNotOptimize(book.apply_update(upd, &changes));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBookApplyUpdate);

/// Сделка по цене нашей заявки с пересчётом очереди.
void BM_QueueOnTrade(benchmark::State& state) {
    asmm::ExchangeConfig cfg;
    asmm::PaperExchange ex(cfg);
    ex.set_level_source([](asmm::Side, double) { return 1e9; });
    ex.set_quotes(75000.0, 75000.5, 0.001, 0.001, 0);
    const asmm::Trade trade{75000.0, 0.01, true, 1, 0};
    for (auto _ : state) {
        benchmark::DoNotOptimize(ex.on_trade(trade));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_QueueOnTrade);

/// Эксперимент из §3.3 статьи: 1000 траекторий × 200 шагов, две стратегии.
void BM_AsPaperSimulation(benchmark::State& state) {
    asmm::AsSimParams p;
    for (auto _ : state) {
        benchmark::DoNotOptimize(asmm::run_as_simulation(p));
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(p.paths));
}
BENCHMARK(BM_AsPaperSimulation)->Unit(benchmark::kMillisecond);

}  // namespace
