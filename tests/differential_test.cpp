// The engine against the reference model on random streams: every event of every command, and both books' state
// at checkpoints, in several mixes of orders. A difference fails the test with the shrunk commands that show it, as
// a scenario file; saved under tests/scenarios/, it stays a regression test. The engine's events also go through the
// Audit, and its state through check_book, which share no code with either book.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "orderbook/book.hpp"
#include "orderbook/check.hpp"
#include "orderbook/differential.hpp"
#include "orderbook/random_flow.hpp"
#include "orderbook/reference_book.hpp"

namespace orderbook {
namespace {

std::string joined(const std::vector<std::string>& lines) {
  std::string text;
  for (const std::string& line : lines) text += line + "\n";
  return text;
}

struct Mix {
  std::uint64_t seed;
  FlowOptions flow;
  std::size_t commands;
  std::size_t state_every;  // how often the books' states are compared, and check_book runs on the engine
  std::size_t min_largest;  // the book must reach this many resting orders at some point, or the mix missed its aim
                            // (each is well below what its stream reached when written)
};

// Runs the stream through both books, stopping at the first difference with the shrunk scenario that shows it.
void expect_same_books(const Mix& mix) {
  RandomFlow flow(mix.seed, mix.flow);
  Lockstep<ReferenceBook, Book> books;
  Audit audit;
  std::vector<Command> commands;
  commands.reserve(mix.commands);
  std::size_t largest = 0;
  for (std::size_t i = 1; i <= mix.commands; ++i) {
    commands.push_back(flow.next());
    std::optional<std::string> what = books.apply(commands.back());
    const bool checkpoint = i % mix.state_every == 0 || i == mix.commands;
    if (!what && checkpoint) what = books.compare();
    if (what) {
      FAIL() << "seed " << mix.seed << ", command " << i << ": " << *what << "\n"
             << shrink_to_scenario<ReferenceBook, Book>(commands);
    }
    audit.record(commands.back(), books.actual_events());
    ASSERT_TRUE(audit.ok()) << "seed " << mix.seed << "\n" << joined(audit.problems());
    if (checkpoint) {
      ASSERT_EQ(joined(check_book(books.actual())), "") << "seed " << mix.seed << ", command " << i;
    }
    largest = std::max(largest, books.actual().size());
  }
  audit.compare(books.actual());
  EXPECT_TRUE(audit.ok()) << joined(audit.problems());
  // The stream did build the book it was meant to (not, say, reject everything).
  EXPECT_GE(largest, mix.min_largest) << "seed " << mix.seed;
}

TEST(Differential, TheEngineMatchesTheReferenceOnTheDefaultMix) {
  for (std::uint64_t seed = 1; seed <= 4; ++seed) expect_same_books({seed, FlowOptions{}, 100'000, 1'000, 250});
}

TEST(Differential, TheEngineMatchesTheReferenceOnLongQueuesAtFewPrices) {
  // Three ticks of range and small quantities: time priority decides nearly every trade, and a taker often fills
  // part of a maker. States are compared after every command.
  expect_same_books({11, FlowOptions{.price_range = 3, .max_quantity = 20, .target_resting = 200}, 50'000, 1, 50});
}

TEST(Differential, TheEngineMatchesTheReferenceWhenEveryOrderIsOneLot) {
  // Every trade fills its maker, so levels empty and are made again all the time.
  expect_same_books({12, FlowOptions{.price_range = 5, .max_quantity = 1, .target_resting = 50}, 50'000, 1, 20});
}

TEST(Differential, TheEngineMatchesTheReferenceWithLargeQuantitiesOnANearlyEmptyBook) {
  // Market orders and IOCs often find the other side empty, and quantities go up to 10^9.
  expect_same_books({13, FlowOptions{.max_quantity = 1'000'000'000, .target_resting = 5}, 50'000, 1, 3});
}

TEST(Differential, TheEngineMatchesTheReferenceWithManyInvalidOrders) {
  // 30% spoiled, half of those sent again fixed under the same id: validation and its order, on both books.
  expect_same_books({14, FlowOptions{.target_resting = 100, .invalid_per_mille = 300}, 50'000, 100, 50});
}

TEST(Differential, TheEngineMatchesTheReferenceOnADeepBookOfManyLevels) {
  // A target of 5,000 orders over 2,000 ticks either side of the mid: it reached 3,157 orders, with 963 bid and 918
  // ask levels at the end, so takers sweep through many levels and cancels hit levels far from the top.
  expect_same_books({15, FlowOptions{.price_range = 2'000, .target_resting = 5'000}, 60'000, 10'000, 2'500});
}

TEST(Differential, TheEngineMatchesTheReferenceAtThePriceBounds) {
  // The mid starts at the top of the price range and is moved in just far enough: orders at and next to 10^9.
  expect_same_books({16, FlowOptions{.start_price = 1'000'000'000, .price_range = 20, .target_resting = 100}, 30'000,
                     100, 50});
  // And at the bottom, next to 1.
  expect_same_books({17, FlowOptions{.start_price = 1, .price_range = 20, .target_resting = 100}, 30'000, 100, 50});
}

}  // namespace
}  // namespace orderbook
