#pragma once

// A resting order as a book reports it (`orders(side)`). The engine and the reference model both report their state
// this way, so the invariant checks and the differential tests can compare them.

#include "orderbook/types.hpp"

namespace orderbook {

struct RestingOrder {
  OrderId id;
  Side side = Side::Buy;
  Price price;
  Quantity remaining;

  bool operator==(const RestingOrder&) const = default;
};

}  // namespace orderbook
