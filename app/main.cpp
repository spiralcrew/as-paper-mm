/**
 * @file main.cpp
 * @brief CLI `asmm`: читает поток сообщений Binance (stdin или файл) и ведёт paper market making.
 *
 * Живой режим:
 * @code
 * python scripts/binance_feed.py | ./build/asmm
 * @endcode
 * Реплей записанных данных:
 * @code
 * ./build/asmm --input data/sample_btcusdt.jsonl
 * @endcode
 */
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "asmm/binance_parser.hpp"
#include "asmm/engine.hpp"

namespace {

std::atomic<bool> g_stop{false};

extern "C" void handle_signal(int /*signal*/) {
    g_stop.store(true);
}

struct Options {
    std::string input = "-";
    asmm::EngineConfig config;
    double report_sec = 10.0;
    bool print_fills = true;
};

void print_usage() {
    std::puts(
        "Usage: asmm [options]\n"
        "Reads Binance Spot JSON messages (bookTicker + trade) line by line and runs\n"
        "an Avellaneda-Stoikov market maker with simulated (paper) execution.\n\n"
        "  --input FILE        input file, '-' = stdin (default)\n"
        "  --gamma X           risk aversion, 1/USDT (default 0.001)\n"
        "  --k X               order arrival decay, 1/USDT (default 2.0)\n"
        "  --horizon SEC       session length T, seconds (default 30)\n"
        "  --qty BTC           order size = 1 lot (default 0.001)\n"
        "  --max-inv LOTS      inventory limit in lots (default 5)\n"
        "  --tick X            price tick (default 0.01)\n"
        "  --sigma0 X          initial sigma, USDT/sqrt(s) (default 2.0)\n"
        "  --vol-half-life SEC EWMA volatility half-life (default 60)\n"
        "  --fill-model M      touch | through (default touch)\n"
        "  --fee X             maker fee rate, e.g. 0.001 = 0.1% (default 0)\n"
        "  --report SEC        status line period in data time (default 10, 0 = off)\n"
        "  --quiet-fills       do not print individual fills\n"
        "  -h, --help          show this help");
}

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto value = [&]() -> const char* {
            if (i + 1 >= argc) {
                throw std::invalid_argument("missing value for " + std::string(arg));
            }
            return argv[++i];
        };
        if (arg == "-h" || arg == "--help") {
            print_usage();
            std::exit(0);
        } else if (arg == "--input") {
            opt.input = value();
        } else if (arg == "--gamma") {
            opt.config.as.gamma = std::stod(value());
        } else if (arg == "--k") {
            opt.config.as.k = std::stod(value());
        } else if (arg == "--horizon") {
            opt.config.as.horizon_sec = std::stod(value());
        } else if (arg == "--qty") {
            opt.config.order_qty = std::stod(value());
        } else if (arg == "--max-inv") {
            opt.config.max_inventory_lots = std::stod(value());
        } else if (arg == "--tick") {
            opt.config.tick_size = std::stod(value());
        } else if (arg == "--sigma0") {
            opt.config.initial_sigma = std::stod(value());
        } else if (arg == "--vol-half-life") {
            opt.config.vol_half_life_sec = std::stod(value());
        } else if (arg == "--fill-model") {
            const std::string_view m = value();
            if (m == "touch") {
                opt.config.fill_model = asmm::FillModel::Touch;
            } else if (m == "through") {
                opt.config.fill_model = asmm::FillModel::Through;
            } else {
                throw std::invalid_argument("unknown fill model: " + std::string(m));
            }
        } else if (arg == "--fee") {
            opt.config.maker_fee_rate = std::stod(value());
        } else if (arg == "--report") {
            opt.report_sec = std::stod(value());
        } else if (arg == "--quiet-fills") {
            opt.print_fills = false;
        } else {
            throw std::invalid_argument("unknown option: " + std::string(arg));
        }
    }
    return true;
}

void print_status(const asmm::EngineStats& s) {
    std::printf(
        "[%lld] mid=%.2f sigma=%.3f tau=%.1fs r=%.2f bid=%s%.2f ask=%s%.2f q=%+.2f lots "
        "pnl=%+.4f fills=%zu\n",
        static_cast<long long>(s.ts_ms), s.mid, s.sigma, s.tau, s.reservation_price,
        s.bid_active ? "" : "~", s.bid, s.ask_active ? "" : "~", s.ask, s.inventory_lots, s.equity,
        s.fills);
    std::fflush(stdout);
}

void print_summary(const asmm::EngineStats& s, std::size_t lines, std::size_t rejected) {
    std::printf(
        "\n=== Paper trading summary (no real funds) ===\n"
        "lines read        : %zu (unparsed: %zu)\n"
        "book updates      : %zu\n"
        "public trades     : %zu\n"
        "simulated fills   : %zu\n"
        "traded volume     : %.2f USDT\n"
        "final inventory   : %+.6f BTC (%+.2f lots)\n"
        "cash              : %+.4f USDT\n"
        "fees              : %.4f USDT\n"
        "mark-to-market PnL: %+.4f USDT (mid %.2f)\n",
        lines, rejected, s.book_updates, s.trades, s.fills, s.volume_quote, s.inventory_btc,
        s.inventory_lots, s.cash, s.fees, s.equity, s.mid);
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    try {
        parse_args(argc, argv, opt);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n\n", e.what());
        print_usage();
        return 2;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::ifstream file;
    std::istream* in = &std::cin;
    if (opt.input != "-") {
        file.open(opt.input);
        if (!file) {
            std::fprintf(stderr, "error: cannot open %s\n", opt.input.c_str());
            return 1;
        }
        in = &file;
    }
    std::ios::sync_with_stdio(false);

    try {
        asmm::MarketMakerEngine engine(opt.config);
        if (opt.print_fills) {
            engine.set_fill_callback([&engine](const asmm::Fill& f) {
                std::printf("FILL %s %.6f BTC @ %.2f (fee %.6f) | q=%+.2f lots\n",
                            f.side == asmm::Side::Buy ? "BUY " : "SELL", f.qty, f.price, f.fee,
                            engine.stats().inventory_lots);
            });
        }

        const auto report_ms = static_cast<std::int64_t>(opt.report_sec * 1000.0);
        std::int64_t next_report = 0;
        std::size_t lines = 0;
        std::size_t rejected = 0;
        std::string line;
        while (!g_stop.load() && std::getline(*in, line)) {
            ++lines;
            const auto msg = asmm::parse_binance_message(line);
            if (!msg) {
                ++rejected;
                continue;
            }
            engine.on_message(*msg);
            if (report_ms > 0 && engine.now_ms() >= next_report) {
                if (next_report != 0) {
                    print_status(engine.stats());
                }
                next_report = engine.now_ms() + report_ms;
            }
        }
        print_summary(engine.stats(), lines, rejected);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
