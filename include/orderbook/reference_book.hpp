#pragma once

// The specification as code (DESIGN.md, "The reference model"): every rule implemented in the most direct way, so
// it can be checked by reading. It is the oracle the engine is tested against, not a fast book: finding the best
// order scans the whole side, so a fill costs O(n) in the number of resting orders.

#include <cstddef>
#include <optional>
#include <vector>

#include "orderbook/command.hpp"
#include "orderbook/event.hpp"
#include "orderbook/text.hpp"
#include "orderbook/types.hpp"

namespace orderbook {

// A resting order as a book reports it.
struct RestingOrder {
  OrderId id;
  Side side = Side::Buy;
  Price price;
  Quantity remaining;

  bool operator==(const RestingOrder&) const = default;
};

class ReferenceBook {
 public:
  // Applies one command and appends the events it produces to `events`.
  void apply(const Command& command, std::vector<Event>& events);

  // The best resting price on a side (the highest bid or the lowest ask), if the side has any orders.
  std::optional<Price> best_price(Side side) const;
  // A side's price levels, best first.
  std::vector<Level> depth(Side side) const;
  // A side's resting orders in priority order: best price first, then arrival.
  std::vector<RestingOrder> orders(Side side) const;
  // Resting orders on both sides.
  std::size_t size() const { return bids_.size() + asks_.size(); }
  // The highest id accepted so far (0 before any): a new order's id must be greater.
  OrderId last_accepted_id() const { return last_accepted_; }

 private:
  void new_order(const NewOrder& order, std::vector<Event>& events);
  void cancel(const CancelOrder& cancel, std::vector<Event>& events);
  std::optional<RejectReason> validate(const NewOrder& order) const;

  std::vector<RestingOrder>& side_orders(Side side) { return side == Side::Buy ? bids_ : asks_; }
  const std::vector<RestingOrder>& side_orders(Side side) const { return side == Side::Buy ? bids_ : asks_; }

  // Each side's resting orders in arrival order (not sorted by price). Erasing keeps the others in arrival order.
  std::vector<RestingOrder> bids_;
  std::vector<RestingOrder> asks_;
  OrderId last_accepted_{0};
};

}  // namespace orderbook
