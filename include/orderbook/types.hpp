#pragma once

// The vocabulary types every part of the book shares: prices, quantities, order ids, sides and order types.
//
// Prices are whole ticks and quantities whole lots, both integers, so two equal prices always compare equal and a
// sum of quantities is exact. The gateway's protocol uses them as they are (decimal prices are left to clients).
// Each is its own type, so a call that passes a quantity where a price belongs doesn't compile.

#include <compare>
#include <cstdint>
#include <optional>
#include <string_view>

namespace orderbook {

struct Price {
  std::int64_t ticks = 0;

  constexpr auto operator<=>(const Price&) const = default;
};

struct Quantity {
  std::int64_t lots = 0;

  constexpr auto operator<=>(const Quantity&) const = default;

  constexpr Quantity& operator+=(Quantity other) {
    lots += other.lots;
    return *this;
  }
  constexpr Quantity& operator-=(Quantity other) {
    lots -= other.lots;
    return *this;
  }
  friend constexpr Quantity operator+(Quantity a, Quantity b) { return a += b; }
  friend constexpr Quantity operator-(Quantity a, Quantity b) { return a -= b; }
};

struct OrderId {
  std::uint64_t value = 0;

  constexpr auto operator<=>(const OrderId&) const = default;
};

enum class Side : std::uint8_t { Buy, Sell };

// Limit rests whatever doesn't trade at once; ImmediateOrCancel (IOC) trades up to its limit price and cancels the
// rest; Market trades at any price and cancels the rest. Neither of the last two ever rests.
enum class OrderType : std::uint8_t { Limit, ImmediateOrCancel, Market };

constexpr Side opposite(Side side) { return side == Side::Buy ? Side::Sell : Side::Buy; }

// The bounds a new order's fields must fall in (DESIGN.md, "Validation"). With both at 10^9, a price times a
// quantity, and the sum of 10^9 quantities, still fit in an int64 with room to spare.
inline constexpr Price kMinPrice{1};
inline constexpr Price kMaxPrice{1'000'000'000};
inline constexpr Quantity kMinQuantity{1};
inline constexpr Quantity kMaxQuantity{1'000'000'000};

constexpr bool valid_price(Price price) { return kMinPrice <= price && price <= kMaxPrice; }
constexpr bool valid_quantity(Quantity quantity) { return kMinQuantity <= quantity && quantity <= kMaxQuantity; }

// The words the text format (scenario files, the CLI) uses: "buy"/"sell" and "limit"/"ioc"/"market".
std::string_view to_string(Side side);
std::string_view to_string(OrderType type);
std::optional<Side> parse_side(std::string_view word);
std::optional<OrderType> parse_order_type(std::string_view word);

}  // namespace orderbook
