#include "orderbook/types.hpp"

namespace orderbook {

std::string_view to_string(Side side) {
  switch (side) {
    case Side::Buy:
      return "buy";
    case Side::Sell:
      return "sell";
  }
  return "?";
}

std::string_view to_string(OrderType type) {
  switch (type) {
    case OrderType::Limit:
      return "limit";
    case OrderType::ImmediateOrCancel:
      return "ioc";
    case OrderType::Market:
      return "market";
  }
  return "?";
}

std::optional<Side> parse_side(std::string_view word) {
  if (word == "buy") return Side::Buy;
  if (word == "sell") return Side::Sell;
  return std::nullopt;
}

std::optional<OrderType> parse_order_type(std::string_view word) {
  if (word == "limit") return OrderType::Limit;
  if (word == "ioc") return OrderType::ImmediateOrCancel;
  if (word == "market") return OrderType::Market;
  return std::nullopt;
}

}  // namespace orderbook
