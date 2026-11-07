/**
 * @file main.cpp
 * @brief CLI `asmm`: читает поток сообщений Binance (stdin или файл) и ведёт paper market making.
 *
 * Живой режим (данные с биржи, исполнение симулируется):
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
    std::string fills_csv;
    std::string state_csv;
    asmm::EngineConfig config;
    double report_sec = 10.0;
    bool print_fills = true;
    bool final_liquidation = true;
};

void print_usage() {
    std::puts(
        "Usage: asmm [options]\n"
        "Reads Binance Spot JSON messages (bookTicker, trade, depth, depthSnapshot) line by\n"
        "line and runs an Avellaneda-Stoikov market maker with simulated (paper) execution.\n"
        "No real orders are sent and no real funds are used.\n\n"
        "Model:\n"
        "  --gamma X           risk aversion, 1/USDT (default 0.001)\n"
        "  --k X               order arrival decay, 1/USDT, used until calibrated (default 2.0)\n"
        "  --no-calibrate      keep --k fixed instead of fitting lambda(d) = A exp(-k d)\n"
        "  --k-min X, --k-max X  bounds for calibrated k (default 0.1, 20)\n"
        "  --horizon SEC       session length T, seconds (default 30)\n"
        "  --sigma0 X          initial sigma, USDT/sqrt(s) (default 2.0)\n"
        "  --vol-half-life SEC EWMA volatility half-life (default 60)\n"
        "Orders and risk:\n"
        "  --qty BTC           order size = 1 lot (default 0.001)\n"
        "  --max-inv LOTS      inventory limit in lots (default 5)\n"
        "  --tick X            price tick (default 0.01)\n"
        "  --requote-ticks N   keep resting order unless price moves >= N ticks (default 1)\n"
        "  --liquidate-session flatten inventory with a market order at each session end\n"
        "  --no-final-liquidation  do not flatten inventory at the end of input\n"
        "Execution simulation:\n"
        "  --fill-model M      queue | touch | through (default queue)\n"
        "  --cancel-model M    proportional | pessimistic (default proportional)\n"
        "  --latency-ms N      order entry/cancel latency, ms (default 0)\n"
        "  --fee X             maker fee rate, e.g. 0.001 = 0.1% (default 0)\n"
        "  --taker-fee X       taker fee rate (default 0.001)\n"
        "Input/output:\n"
        "  --input FILE        input file, '-' = stdin (default)\n"
        "  --report SEC        status line period in data time (default 10, 0 = off)\n"
        "  --quiet-fills       do not print individual fills\n"
        "  --fills-csv FILE    write every fill (maker and taker) to CSV\n"
        "  --state-csv FILE    write engine state every report period to CSV\n"
        "  -h, --help          show this help");
}

double to_number(const char* text) {
    return std::stod(text);
}

void parse_args(int argc, char** argv, Options& opt) {
    asmm::EngineConfig& c = opt.config;
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
            c.as.gamma = to_number(value());
        } else if (arg == "--k") {
            c.as.k = to_number(value());
        } else if (arg == "--no-calibrate") {
            c.calibrate_k = false;
        } else if (arg == "--k-min") {
            c.k_min = to_number(value());
        } else if (arg == "--k-max") {
            c.k_max = to_number(value());
        } else if (arg == "--horizon") {
            c.as.horizon_sec = to_number(value());
        } else if (arg == "--sigma0") {
            c.initial_sigma = to_number(value());
        } else if (arg == "--vol-half-life") {
            c.vol_half_life_sec = to_number(value());
        } else if (arg == "--qty") {
            c.order_qty = to_number(value());
        } else if (arg == "--max-inv") {
            c.max_inventory_lots = to_number(value());
        } else if (arg == "--tick") {
            c.tick_size = to_number(value());
        } else if (arg == "--requote-ticks") {
            c.requote_ticks = std::stoi(value());
        } else if (arg == "--liquidate-session") {
            c.liquidate_at_session_end = true;
        } else if (arg == "--no-final-liquidation") {
            opt.final_liquidation = false;
        } else if (arg == "--fill-model") {
            const std::string_view m = value();
            if (m == "queue") {
                c.exchange.fill_model = asmm::FillModel::Queue;
            } else if (m == "touch") {
                c.exchange.fill_model = asmm::FillModel::Touch;
            } else if (m == "through") {
                c.exchange.fill_model = asmm::FillModel::Through;
            } else {
                throw std::invalid_argument("unknown fill model: " + std::string(m));
            }
        } else if (arg == "--cancel-model") {
            const std::string_view m = value();
            if (m == "proportional") {
                c.exchange.cancel_model = asmm::CancelModel::Proportional;
            } else if (m == "pessimistic") {
                c.exchange.cancel_model = asmm::CancelModel::Pessimistic;
            } else {
                throw std::invalid_argument("unknown cancel model: " + std::string(m));
            }
        } else if (arg == "--latency-ms") {
            c.exchange.latency_ms = std::stoll(value());
        } else if (arg == "--fee") {
            c.exchange.maker_fee_rate = to_number(value());
        } else if (arg == "--taker-fee") {
            c.exchange.taker_fee_rate = to_number(value());
        } else if (arg == "--report") {
            opt.report_sec = to_number(value());
        } else if (arg == "--quiet-fills") {
            opt.print_fills = false;
        } else if (arg == "--fills-csv") {
            opt.fills_csv = value();
        } else if (arg == "--state-csv") {
            opt.state_csv = value();
        } else {
            throw std::invalid_argument("unknown option: " + std::string(arg));
        }
    }
}

void print_status(const asmm::EngineStats& s) {
    std::printf(
        "[%lld] mid=%.2f sigma=%.3f k=%.3f%s tau=%.1fs r=%.2f bid=%s%.2f(q%.3f) "
        "ask=%s%.2f(q%.3f) inv=%+.2f lots pnl=%+.4f fills=%zu book=%s\n",
        static_cast<long long>(s.ts_ms), s.mid, s.sigma, s.k, s.k_calibrated ? "*" : "", s.tau,
        s.reservation_price, s.bid_active ? "" : "~", s.bid, s.bid_queue, s.ask_active ? "" : "~",
        s.ask, s.ask_queue, s.inventory_lots, s.equity, s.fills,
        s.book_synced ? "synced" : "top-only");
    std::fflush(stdout);
}

void write_state(std::ofstream& out, const asmm::EngineStats& s) {
    out << s.ts_ms << ',' << s.mid << ',' << s.sigma << ',' << s.k << ',' << s.intensity_A << ','
        << s.tau << ',' << s.reservation_price << ',' << s.spread << ',' << s.bid << ',' << s.ask
        << ',' << s.bid_queue << ',' << s.ask_queue << ',' << s.inventory_lots << ',' << s.cash
        << ',' << s.equity << ',' << s.fills << ',' << (s.book_synced ? 1 : 0) << '\n';
}

void print_summary(const asmm::EngineStats& s, std::size_t lines, std::size_t rejected) {
    const double net = s.equity;
    std::printf(
        "\n=== Paper trading summary (simulated execution, no real funds) ===\n"
        "lines read          : %zu (unparsed: %zu)\n"
        "bookTicker updates  : %zu\n"
        "depth snapshots     : %zu, updates applied: %zu, gaps: %zu, synced: %s\n"
        "public trades       : %zu\n"
        "k used / fit (R2)   : %.4f / %.4f 1/USDT (%.3f), A = %.4f 1/s (%s)\n"
        "maker fills         : %zu, volume %.2f USDT\n"
        "edge captured vs mid: %+.4f USDT\n"
        "taker orders        : %zu, volume %.2f USDT, slippage %.4f USDT\n"
        "fees                : %.4f USDT\n"
        "inventory           : %+.6f BTC (%+.2f lots, max |q| %.2f lots)\n"
        "cash                : %+.4f USDT\n"
        "net PnL (mark-to-mid): %+.4f USDT (mid %.2f)\n",
        lines, rejected, s.book_updates, s.depth_snapshots, s.depth_updates, s.depth_gaps,
        s.book_synced ? "yes" : "no", s.trades, s.k, s.k_fit, s.k_fit_r2, s.intensity_A,
        s.k_calibrated ? "calibrated" : "no reliable fit yet, using --k", s.fills, s.volume_quote,
        s.edge_captured, s.taker_orders, s.taker_volume, s.slippage_cost, s.fees, s.inventory_btc,
        s.inventory_lots, s.max_abs_inventory_lots, s.cash, net, s.mid);
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

    std::ofstream fills_csv;
    if (!opt.fills_csv.empty()) {
        fills_csv.open(opt.fills_csv);
        fills_csv << "ts_ms,type,side,price,qty,fee,mid,slippage\n";
    }
    std::ofstream state_csv;
    if (!opt.state_csv.empty()) {
        state_csv.open(opt.state_csv);
        state_csv << "ts_ms,mid,sigma,k,A,tau,reservation,spread,bid,ask,bid_queue,ask_queue,"
                     "inventory_lots,cash,equity,fills,book_synced\n";
        state_csv.precision(10);
    }
    fills_csv.precision(10);

    try {
        asmm::MarketMakerEngine engine(opt.config);
        engine.set_fill_callback([&](const asmm::Fill& f) {
            const bool buy = f.side == asmm::Side::Buy;
            if (opt.print_fills) {
                std::printf("FILL  maker %s %.6f BTC @ %.2f (fee %.6f)\n", buy ? "BUY " : "SELL",
                            f.qty, f.price, f.fee);
            }
            if (fills_csv.is_open()) {
                fills_csv << f.ts_ms << ",maker," << (buy ? "buy" : "sell") << ',' << f.price << ','
                          << f.qty << ',' << f.fee << ",,\n";
            }
        });
        engine.set_taker_callback([&](const asmm::MarketFill& m) {
            const bool buy = m.side == asmm::Side::Buy;
            std::printf(
                "TAKER %s %.6f BTC vwap %.2f (mid %.2f, slippage %.4f USDT, %zu levels%s)\n",
                buy ? "BUY " : "SELL", m.qty, m.vwap, m.mid, m.slippage, m.levels,
                m.complete ? "" : ", book exhausted");
            if (fills_csv.is_open()) {
                fills_csv << m.ts_ms << ",taker," << (buy ? "buy" : "sell") << ',' << m.vwap << ','
                          << m.qty << ',' << m.fee << ',' << m.mid << ',' << m.slippage << '\n';
            }
        });

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
                    const auto s = engine.stats();
                    print_status(s);
                    if (state_csv.is_open()) {
                        write_state(state_csv, s);
                    }
                }
                next_report = engine.now_ms() + report_ms;
            }
        }
        if (opt.final_liquidation) {
            engine.liquidate();
        }
        print_summary(engine.stats(), lines, rejected);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
