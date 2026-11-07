/**
 * @file order_book.hpp
 * @brief Локальная L2-книга заявок, синхронизируемая по правилам Binance Spot.
 *
 * Алгоритм синхронизации («How to manage a local order book correctly»,
 * https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams):
 * 1. применить снимок REST с `lastUpdateId`;
 * 2. отбросить обновления с `u <= lastUpdateId`;
 * 3. если у обновления `U > lastUpdateId + 1` — пропущены события, книга рассинхронизирована,
 *    нужен новый снимок;
 * 4. иначе применить уровни (объём 0 — удалить уровень) и положить `lastUpdateId = u`.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "asmm/types.hpp"

namespace asmm {

/// Результат применения инкрементального обновления.
enum class BookUpdateResult : std::uint8_t {
    Applied,    ///< применено
    Stale,      ///< устарело (`u <= lastUpdateId`), пропущено
    Gap,        ///< пропуск в последовательности, книга рассинхронизирована
    NoSnapshot  ///< нет актуального снимка, обновление нельзя применить
};

/// Изменение уровня книги (новый абсолютный объём).
struct LevelChange {
    Side side = Side::Buy;  ///< `Buy` — биды, `Sell` — аски
    double price = 0.0;     ///< цена уровня
    double qty = 0.0;       ///< новый объём (0 — уровень удалён)
};

/**
 * @brief L2-книга: цена → суммарный объём. Цены хранятся в целых тиках, чтобы сравнения
 * уровней были точными.
 */
class OrderBook {
public:
    /// @throws std::invalid_argument при tick_size <= 0
    explicit OrderBook(double tick_size = 0.01);

    /// Заменить книгу снимком и начать синхронизацию.
    void apply_snapshot(const DepthSnapshot& snapshot);

    /**
     * @brief Применить обновление по правилам Binance.
     * @param changes если не `nullptr`, сюда дописываются изменившиеся уровни
     */
    BookUpdateResult apply_update(const DepthUpdate& update,
                                  std::vector<LevelChange>* changes = nullptr);

    /// Очистить книгу (до следующего снимка).
    void reset() noexcept;

    /// Книга синхронизирована и может использоваться.
    [[nodiscard]] bool synced() const noexcept { return synced_; }

    /// `lastUpdateId` последнего применённого события.
    [[nodiscard]] std::uint64_t last_update_id() const noexcept { return last_update_id_; }

    /// Время последнего применённого события, мс.
    [[nodiscard]] std::int64_t ts_ms() const noexcept { return ts_ms_; }

    [[nodiscard]] std::optional<PriceLevel> best_bid() const noexcept;  ///< лучший бид
    [[nodiscard]] std::optional<PriceLevel> best_ask() const noexcept;  ///< лучший аск

    /// Лучшие цены в формате `BookTop`; `nullopt`, если стороны пусты.
    [[nodiscard]] std::optional<BookTop> top() const noexcept;

    /// Объём на уровне `price` стороны `side` (0, если уровня нет).
    [[nodiscard]] double qty_at(Side side, double price) const noexcept;

    /// Число уровней на стороне.
    [[nodiscard]] std::size_t levels(Side side) const noexcept;

    /// До `n` лучших уровней стороны, от лучшего к худшему.
    [[nodiscard]] std::vector<PriceLevel> top_levels(Side side, std::size_t n) const;

    /// Шаг цены.
    [[nodiscard]] double tick_size() const noexcept { return tick_size_; }

private:
    [[nodiscard]] std::int64_t to_ticks(double price) const noexcept;
    [[nodiscard]] double to_price(std::int64_t ticks) const noexcept;
    void set_level(Side side, double price, double qty, std::vector<LevelChange>* changes);

    double tick_size_;
    std::map<std::int64_t, double, std::greater<>> bids_;
    std::map<std::int64_t, double> asks_;
    std::uint64_t last_update_id_ = 0;
    std::int64_t ts_ms_ = 0;
    bool synced_ = false;
};

}  // namespace asmm
