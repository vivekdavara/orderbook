#pragma once

// What the book answers (DESIGN.md, "Events"). For each command, in order: a rejection; or an acceptance, the trades,
// and for an IOC or market order that didn't fill, its cancellation; or, for a cancel, the order's cancellation.

#include <cstdint>
#include <variant>

#include "orderbook/types.hpp"

namespace orderbook {

// In the order validation checks them: the first rule an order breaks is its reason.
enum class RejectReason : std::uint8_t { IdNotIncreasing, InvalidQuantity, InvalidPrice, UnknownOrder };

enum class CancelReason : std::uint8_t {
  Requested,  // a cancel command removed a resting order
  Unfilled,   // what an IOC or market order couldn't trade at once
};

struct Accepted {
  OrderId id;

  bool operator==(const Accepted&) const = default;
};

struct Rejected {
  OrderId id;
  RejectReason reason = RejectReason::UnknownOrder;

  bool operator==(const Rejected&) const = default;
};

// One fill between the incoming order (the taker) and a resting one (the maker), at the maker's price.
struct Trade {
  OrderId taker;
  OrderId maker;
  Quantity quantity;
  Price price;

  bool operator==(const Trade&) const = default;
};

struct Cancelled {
  OrderId id;
  Quantity remaining;  // what was left of the order when it was cancelled
  CancelReason reason = CancelReason::Requested;

  bool operator==(const Cancelled&) const = default;
};

using Event = std::variant<Accepted, Rejected, Trade, Cancelled>;

}  // namespace orderbook
