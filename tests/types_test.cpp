#include "orderbook/types.hpp"

#include <gtest/gtest.h>

#include <type_traits>

namespace orderbook {
namespace {

TEST(Types, PricesQuantitiesAndIdsAreDistinctTypes) {
  // The point of wrapping the integers: none converts to another, or to or from a plain integer.
  static_assert(!std::is_convertible_v<Price, Quantity>);
  static_assert(!std::is_convertible_v<Quantity, Price>);
  static_assert(!std::is_convertible_v<OrderId, Price>);
  static_assert(!std::is_convertible_v<std::int64_t, Price>);
  static_assert(!std::is_convertible_v<Price, std::int64_t>);
  // They stay plain values the engine can copy with memcpy.
  static_assert(std::is_trivially_copyable_v<Price>);
  static_assert(std::is_trivially_copyable_v<Quantity>);
  static_assert(std::is_trivially_copyable_v<OrderId>);
  static_assert(sizeof(Price) == 8 && sizeof(Quantity) == 8 && sizeof(OrderId) == 8);
}

TEST(Types, CompareByValue) {
  EXPECT_LT(Price{100}, Price{101});
  EXPECT_EQ(Price{100}, Price{100});
  EXPECT_GT(Quantity{5}, Quantity{4});
  EXPECT_LT(OrderId{1}, OrderId{2});
}

TEST(Types, QuantityArithmeticIsExact) {
  Quantity q{10};
  q -= Quantity{3};
  EXPECT_EQ(q, Quantity{7});
  q += Quantity{5};
  EXPECT_EQ(q, Quantity{12});
  EXPECT_EQ(Quantity{2} + Quantity{3}, Quantity{5});
  EXPECT_EQ(Quantity{2} - Quantity{3}, Quantity{-1});
  // The largest sum the book can hold: 10^9 orders each at the maximum quantity still fits in an int64.
  static_assert(kMaxQuantity.lots * 1'000'000'000 / 1'000'000'000 == kMaxQuantity.lots);
}

TEST(Types, OppositeSide) {
  EXPECT_EQ(opposite(Side::Buy), Side::Sell);
  EXPECT_EQ(opposite(Side::Sell), Side::Buy);
}

TEST(Types, ValidationBoundsAreInclusive) {
  EXPECT_FALSE(valid_price(Price{0}));
  EXPECT_FALSE(valid_price(Price{-1}));
  EXPECT_TRUE(valid_price(kMinPrice));
  EXPECT_TRUE(valid_price(kMaxPrice));
  EXPECT_FALSE(valid_price(Price{kMaxPrice.ticks + 1}));

  EXPECT_FALSE(valid_quantity(Quantity{0}));
  EXPECT_FALSE(valid_quantity(Quantity{-5}));
  EXPECT_TRUE(valid_quantity(kMinQuantity));
  EXPECT_TRUE(valid_quantity(kMaxQuantity));
  EXPECT_FALSE(valid_quantity(Quantity{kMaxQuantity.lots + 1}));
}

TEST(Types, WordsRoundTrip) {
  for (Side side : {Side::Buy, Side::Sell}) {
    EXPECT_EQ(parse_side(to_string(side)), side);
  }
  for (OrderType type : {OrderType::Limit, OrderType::ImmediateOrCancel, OrderType::Market}) {
    EXPECT_EQ(parse_order_type(to_string(type)), type);
  }
  EXPECT_EQ(to_string(OrderType::ImmediateOrCancel), "ioc");
}

TEST(Types, UnknownWordsAreRejected) {
  EXPECT_EQ(parse_side("BUY"), std::nullopt);  // the format is lower case only
  EXPECT_EQ(parse_side(""), std::nullopt);
  EXPECT_EQ(parse_side("bid"), std::nullopt);
  EXPECT_EQ(parse_order_type("fok"), std::nullopt);
  EXPECT_EQ(parse_order_type("limit "), std::nullopt);
}

}  // namespace
}  // namespace orderbook
