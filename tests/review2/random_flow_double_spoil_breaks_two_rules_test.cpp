// Review 2: random_flow.hpp promises "A spoiled order breaks one rule, or one in four two", and random_flow.cpp's
// new_order "One in four spoiled orders breaks two rules, so the order of the checks decides the reason". But the
// second spoil is drawn from the six ways, not from the other two fields: when both draws pick the quantity (ways 1
// to 3) or both the price (4 and 5), the second overwrites the first and only one rule is broken. That happens for
// 4 in 15 double spoils, so only 1/4 * 11/15 = 18.3% of spoiled orders break two rules, not 25%.

#include <gtest/gtest.h>

#include <variant>
#include <vector>

#include "orderbook/random_flow.hpp"
#include "orderbook/reference_book.hpp"

namespace orderbook {
namespace {

TEST(Review2RandomFlow, OneInFourSpoiledOrdersBreaksTwoRules) {
  // Every new order is spoiled except resends (which are sent valid), so the rejected new orders are exactly the
  // spoiled ones.
  RandomFlow flow(5, FlowOptions{.target_resting = 100, .invalid_per_mille = 1000});
  ReferenceBook book;
  std::vector<Event> events;
  OrderId last_accepted{0};
  int spoiled = 0, two_rules = 0;
  for (int i = 0; i < 400'000; ++i) {
    const Command command = flow.next();
    events.clear();
    book.apply(command, events);
    const auto* order = std::get_if<NewOrder>(&command);
    if (order == nullptr) continue;
    if (!std::holds_alternative<Rejected>(events.front())) {
      last_accepted = order->id;
      continue;
    }
    ++spoiled;
    const bool bad_id = order->id <= last_accepted;
    const bool bad_quantity = !valid_quantity(order->quantity);
    const bool bad_price = order->type == OrderType::Market ? order->price != Price{0} : !valid_price(order->price);
    if (int{bad_id} + int{bad_quantity} + int{bad_price} == 2) ++two_rules;
  }
  ASSERT_GT(spoiled, 100'000);
  // The standard error of the share is about 0.13 percentage points here; this bound is 15 of them wide.
  EXPECT_NEAR(100.0 * two_rules / spoiled, 25.0, 2.0) << two_rules << " of " << spoiled << " spoiled orders";
}

}  // namespace
}  // namespace orderbook
