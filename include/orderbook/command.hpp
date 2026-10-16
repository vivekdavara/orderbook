#pragma once

// What a client asks the book to do (DESIGN.md, "Commands"). A command is just data: holding one says nothing about
// whether it is valid. Validation is the book's job, and an invalid command is answered with a rejection.

#include <variant>

#include "orderbook/types.hpp"

namespace orderbook {

struct NewOrder {
  OrderId id;
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  Quantity quantity;
  Price price;  // 0 for a market order, which has no price

  bool operator==(const NewOrder&) const = default;
};

struct CancelOrder {
  OrderId id;

  bool operator==(const CancelOrder&) const = default;
};

using Command = std::variant<NewOrder, CancelOrder>;

}  // namespace orderbook
