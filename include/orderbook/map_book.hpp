#pragma once

// Day 2's matching engine, kept as the baseline day 3's engine (`Book`) is measured against, and as a second book the
// tests run every rule on. It is the conventional design, built from standard containers:
//
// - Each side is a std::map from price to that price's level, ordered best first, so the best price is the map's
//   first entry and a taker walks the map from the front.
// - Each level is a FIFO std::list of its orders, oldest first, and keeps its total quantity as it changes, so depth
//   is one entry per level.
// - An index from order id to where the order sits (its level and its node in that level's list) makes a cancel
//   O(1) amortised: no search for the order and none for its level.
//
// Every order that rests allocates a list node and a hash node (and a new level a map node), and matching follows
// pointers through all three: what day 3's engine takes off the hot path. It lives in the test kit, not the engine's
// library: nothing but the tests and benchmarks uses it.

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "orderbook/command.hpp"
#include "orderbook/event.hpp"
#include "orderbook/resting_order.hpp"
#include "orderbook/text.hpp"
#include "orderbook/types.hpp"

namespace orderbook {

class MapBook {
 public:
  MapBook() = default;
  // Move-only. The index holds iterators into this book's own maps and lists, so a member-wise copy would leave the
  // copy's index pointing into the original. Moving keeps them valid: node-based containers hand their nodes over,
  // so every iterator then refers to the same element in the book moved to.
  MapBook(const MapBook&) = delete;
  MapBook& operator=(const MapBook&) = delete;
  MapBook(MapBook&&) = default;
  MapBook& operator=(MapBook&&) = default;
  ~MapBook() = default;

  // Applies one command and appends the events it produces to `events`.
  void apply(const Command& command, std::vector<Event>& events);

  // The best resting price on a side (the highest bid or the lowest ask), if the side has any orders. O(1).
  std::optional<Price> best_price(Side side) const;
  // A side's price levels, best first. O(levels).
  std::vector<Level> depth(Side side) const;
  // A side's resting orders in priority order: best price first, then arrival. O(orders).
  std::vector<RestingOrder> orders(Side side) const;
  // Resting orders on both sides.
  std::size_t size() const { return index_.size(); }
  // The highest id accepted so far (0 before any): a new order's id must be greater.
  OrderId last_accepted_id() const { return last_accepted_; }

 private:
  struct Order {
    OrderId id;
    Quantity remaining;
  };
  struct PriceLevel {
    std::list<Order> queue;  // oldest first: the front is next to trade
    Quantity total;          // the queue's remaining quantities summed, kept as they change
  };
  // Orders prices best first: higher first for bids, lower first for asks. One comparator type for both sides makes
  // both sides the same map type, so the index can hold an iterator into either.
  struct BetterPrice {
    Side side = Side::Buy;
    bool operator()(Price a, Price b) const { return side == Side::Buy ? a > b : a < b; }
  };
  using Ladder = std::map<Price, PriceLevel, BetterPrice>;
  struct Location {
    Side side;
    Ladder::iterator level;
    std::list<Order>::iterator order;
  };

  void new_order(const NewOrder& order, std::vector<Event>& events);
  void cancel(const CancelOrder& cancel, std::vector<Event>& events);
  std::optional<RejectReason> validate(const NewOrder& order) const;
  void rest(const NewOrder& order, Quantity remaining);

  Ladder& ladder(Side side) { return side == Side::Buy ? bids_ : asks_; }
  const Ladder& ladder(Side side) const { return side == Side::Buy ? bids_ : asks_; }

  Ladder bids_{BetterPrice{Side::Buy}};
  Ladder asks_{BetterPrice{Side::Sell}};
  std::unordered_map<std::uint64_t, Location> index_;  // every resting order, by id
  OrderId last_accepted_{0};
};

}  // namespace orderbook
