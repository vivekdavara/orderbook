// Review finding: RandomFlow sends invalid orders as valid ones when start_price is near a price bound.
//
// src/random_flow.cpp:36 promises the mid stays "far enough from both price bounds that every price this stream
// means to be valid is", and random_flow.hpp:54 that last_valid_id_ is an id "the book has therefore accepted". But
// the clamp only happens inside step_mid(), and only on the 1 in 4 calls that move the mid; the constructor takes
// options.start_price as it is (random_flow.cpp:8). So with start_price = 10 and the default range of 50, the first
// commands, before the first clamp, are limit buys priced as low as 10 - 50 = -40. The stream counts them as valid,
// makes them cancel candidates and records their ids in last_valid_id_.
//
// With invalid_per_mille = 0 the stream spoils nothing, so every new order it sends must be accepted.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "orderbook/random_flow.hpp"
#include "orderbook/reference_book.hpp"
#include "orderbook/text.hpp"

namespace orderbook {
namespace {

TEST(ReviewRandomFlow, AStreamWithNothingSpoiledSendsOnlyValidOrdersFromAnyStartPrice) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    RandomFlow flow(seed, FlowOptions{.start_price = 10, .invalid_per_mille = 0});
    ReferenceBook book;
    std::vector<Event> events;
    for (int i = 0; i < 200; ++i) {
      const Command command = flow.next();
      events.clear();
      book.apply(command, events);
      if (!std::holds_alternative<NewOrder>(command)) continue;
      ASSERT_FALSE(std::holds_alternative<Rejected>(events.front()))
          << "seed " << seed << ", command " << i << ": '" << format(command) << "' gave '" << format(events.front())
          << "'";
    }
  }
}

}  // namespace
}  // namespace orderbook
