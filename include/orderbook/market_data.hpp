#pragma once

// Market data (DESIGN.md, "Market data (day 4)"): the book by price level, as the outside world sees it. It is built
// from the engine's events, not read from the engine, so the engine's thread does no extra work and the feed runs
// wherever the events go (the gateway's thread).
//
// - `DepthFeed`, the publisher, follows each command and the events a book answered it with. It keeps its own record
//   of the resting orders (side, price, what is left) and each level's total, and says which levels the command
//   changed: each once, with its new total, 0 when the level emptied. It numbers the commands it follows from 1: the
//   feed's sequence.
// - `L2Snapshot`: the top of the book after one command, up to K levels a side, best first.
// - `L2Book`, a subscriber: it starts empty at sequence 0, or from a snapshot, and applies each command's updates in
//   sequence. Updates that don't follow the last ones applied are a gap: refused, and the subscriber starts again from
//   a snapshot. Starting from a top-K snapshot follows only those levels correctly; a full one (`kAllLevels`) follows
//   them all.
//
// The feed's levels equal the engine's depth after every command: tests/market_data_test.cpp checks it on random
// streams, with a subscriber following the feed.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <unordered_map>
#include <vector>

#include "orderbook/command.hpp"
#include "orderbook/event.hpp"
#include "orderbook/text.hpp"
#include "orderbook/types.hpp"

namespace orderbook {

// A level's total after a command; 0 means the level is gone.
struct LevelUpdate {
  Side side = Side::Buy;
  Price price;
  Quantity total;

  bool operator==(const LevelUpdate&) const = default;
};

// The top of the book after command `sequence` (0: before any): up to the levels asked for, best first.
struct L2Snapshot {
  std::uint64_t sequence = 0;
  std::vector<Level> bids;
  std::vector<Level> asks;

  bool operator==(const L2Snapshot&) const = default;
};

inline constexpr std::size_t kAllLevels = std::numeric_limits<std::size_t>::max();

// "level buy 101x10" (a level's new total; "x0" when it is gone) and "snapshot 7 bids 101x10 100x5 asks 102x7"
// (with "-" for an empty side): the text forms. `format_updates` is one command's updates on a line, "update 8 sell
// 101x0 buy 101x3", or "update 8" when it changed no level: what `orderbook replay --updates` prints and the gateway
// sends its subscribers, a line for every command, so a missing one shows as a gap in the sequence.
std::string format(const LevelUpdate& update);
std::string format(const L2Snapshot& snapshot);
std::string format_updates(std::uint64_t sequence, std::span<const LevelUpdate> updates);

// One side's levels and their totals, best first: bids highest first, asks lowest first.
class LevelTotals {
 public:
  explicit LevelTotals(Side side) : levels_(BetterPrice{side}) {}

  // Adds `change` to the level at `price` (negative to take away); a level whose total reaches 0 is removed.
  void add(Price price, Quantity change);
  void set(Price price, Quantity total);  // 0 removes the level
  Quantity total(Price price) const;
  std::vector<Level> top(std::size_t levels) const;
  std::size_t size() const { return levels_.size(); }

 private:
  struct BetterPrice {
    Side side;
    bool operator()(Price a, Price b) const { return side == Side::Buy ? a > b : a < b; }
  };
  std::map<Price, Quantity, BetterPrice> levels_;
};

class DepthFeed {
 public:
  // Follows one command, given the events a book answered it with, and appends to `updates` the levels it changed:
  // each once, in the order first changed, with its total after the command. Returns the command's sequence number.
  // Throws std::logic_error if the events don't fit the feed's record (a trade with, or a cancellation of, an order
  // it doesn't have resting, at another price or for more than is left of it): the book and its feed disagree, and
  // the feed can't be used any more, as an engine that can't say what it did shouldn't go on matching.
  std::uint64_t apply(const Command& command, std::span<const Event> events, std::vector<LevelUpdate>& updates);

  // Up to `levels` levels a side, best first, after the last command followed.
  L2Snapshot snapshot(std::size_t levels) const;
  // A side's whole depth, best first, as a book's depth() gives it.
  std::vector<Level> depth(Side side) const { return ladder(side).top(kAllLevels); }
  std::uint64_t sequence() const { return sequence_; }
  // Resting orders, by the feed's record.
  std::size_t resting() const { return orders_.size(); }

 private:
  struct Order {
    Side side = Side::Buy;
    Price price;
    Quantity remaining;
  };

  void follow_new_order(const NewOrder& order, std::span<const Event> events, std::vector<LevelUpdate>& updates,
                        std::size_t first);
  void follow_cancel(const CancelOrder& cancel, std::span<const Event> events, std::vector<LevelUpdate>& updates,
                     std::size_t first);
  // Takes `quantity` from resting order `id` (which must have it, at `price`), and from its level.
  void take(OrderId id, Side side, Price price, Quantity quantity, std::vector<LevelUpdate>& updates,
            std::size_t first);

  LevelTotals& ladder(Side side) { return side == Side::Buy ? bids_ : asks_; }
  const LevelTotals& ladder(Side side) const { return side == Side::Buy ? bids_ : asks_; }

  std::unordered_map<std::uint64_t, Order> orders_;  // resting orders by id
  LevelTotals bids_{Side::Buy};
  LevelTotals asks_{Side::Sell};
  std::uint64_t sequence_ = 0;
};

class L2Book {
 public:
  L2Book() = default;  // empty, at sequence 0: follows a feed from its first command
  explicit L2Book(const L2Snapshot& snapshot);

  // Applies command `sequence`'s updates. Returns false, changing nothing, unless `sequence` is the one after the last
  // applied: a gap (or a repeat), after which the book must start again from a snapshot.
  bool apply(std::uint64_t sequence, std::span<const LevelUpdate> updates);

  std::vector<Level> depth(Side side) const { return (side == Side::Buy ? bids_ : asks_).top(kAllLevels); }
  L2Snapshot snapshot(std::size_t levels) const;
  std::uint64_t sequence() const { return sequence_; }

 private:
  LevelTotals bids_{Side::Buy};
  LevelTotals asks_{Side::Sell};
  std::uint64_t sequence_ = 0;
};

}  // namespace orderbook
