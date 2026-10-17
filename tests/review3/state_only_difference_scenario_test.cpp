// Review 3. DESIGN.md ("Differential tests") says a difference only in the state "shrinks by the state; the file's
// depth lines then catch it unless it is in the order of a queue", README ("When a random run fails") says the same,
// and tools/orderbook_cli.cpp:12-14 promises that when diff finds a difference, "`orderbook check` of that file fails
// on the engine and passes on the reference model". But differential_scenario (differential.hpp:121-141) writes only
// events and the final depth, while state_difference (differential.hpp:36-62) also compares the best prices, the
// number resting and the last accepted id. A state-only difference in any of those shrinks to a file whose events and
// depth are all right, so the broken book passes it: the file is not a regression test for the bug it reports.
//
// Each test below shrinks a real state-only difference, checks the harness did find it (so the failure is the
// file's, not the search's), and then asks for what the docs promise: the broken book fails the file.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "../broken_books.hpp"
#include "orderbook/differential.hpp"
#include "orderbook/random_flow.hpp"
#include "orderbook/reference_book.hpp"
#include "orderbook/scenario.hpp"
#include "orderbook/text.hpp"

namespace orderbook {
namespace {

// The reference model with its best prices cached, as day 3's "levels in a structure that keeps the best price cheap
// to find" (DESIGN.md) might: refreshed after every new order, but not after a cancel. Every event and every depth
// is the reference model's; only best_price() goes stale once a cancel removes the best level.
class BookWithAStaleBestPrice : public broken::WrappedReferenceBook {
 public:
  void apply(const Command& command, std::vector<Event>& events) {
    book_.apply(command, events);
    if (std::holds_alternative<NewOrder>(command)) {
      bid_ = book_.best_price(Side::Buy);
      ask_ = book_.best_price(Side::Sell);
    }
  }
  std::optional<Price> best_price(Side side) const { return side == Side::Buy ? bid_ : ask_; }

 private:
  std::optional<Price> bid_;
  std::optional<Price> ask_;
};

// Reports the newest id it was sent, accepted or not, as the last accepted id. Validation itself (the reference
// model's) is right, so every event is too.
class BookThatReportsRejectedIds : public broken::WrappedReferenceBook {
 public:
  void apply(const Command& command, std::vector<Event>& events) {
    book_.apply(command, events);
    if (const auto* order = std::get_if<NewOrder>(&command); order != nullptr && order->id > newest_) {
      newest_ = order->id;
    }
  }
  OrderId last_accepted_id() const { return newest_ > book_.last_accepted_id() ? newest_ : book_.last_accepted_id(); }

 private:
  OrderId newest_{0};
};

// Counts each maker a trade filled as still resting, as an engine that forgot to erase it from its index would
// (src/book.cpp:61 is that line). Events and depth are the reference model's.
class BookThatCountsFilledMakers : public broken::WrappedReferenceBook {
 public:
  void apply(const Command& command, std::vector<Event>& events) {
    const std::size_t first = events.size();
    book_.apply(command, events);
    for (std::size_t i = first; i < events.size(); ++i) {
      const auto* trade = std::get_if<Trade>(&events[i]);
      if (trade != nullptr && !rests(trade->maker)) ++filled_;
    }
  }
  std::size_t size() const { return book_.size() + filled_; }

 private:
  bool rests(OrderId id) const {
    for (const Side side : {Side::Buy, Side::Sell}) {
      for (const RestingOrder& order : book_.orders(side)) {
        if (order.id == id) return true;
      }
    }
    return false;
  }

  std::size_t filled_ = 0;
};

std::vector<Command> parse_lines(const std::vector<std::string>& lines) {
  std::vector<Command> commands;
  for (const std::string& line : lines) commands.push_back(*parse_command(line));
  return commands;
}

std::vector<Command> random_commands(std::uint64_t seed, std::size_t count, FlowOptions options) {
  RandomFlow flow(seed, options);
  std::vector<Command> commands;
  for (std::size_t i = 0; i < count; ++i) commands.push_back(flow.next());
  return commands;
}

template <typename AnyBook>
bool passes(const std::string& text) {
  const auto scenario = parse_scenario(text);
  EXPECT_TRUE(scenario.has_value()) << text;
  if (!scenario) return false;
  AnyBook book;
  return run_scenario(*scenario, book).empty();
}

// Finds the first state difference (states compared after every command), shrinks the stream up to it, and returns
// the scenario file, after checking the difference was found where expected and is only in the state.
template <typename Broken>
std::string shrunk_state_difference(const std::vector<Command>& commands, const std::string& what) {
  const auto difference = first_difference<ReferenceBook, Broken>(commands, 1);
  EXPECT_TRUE(difference.has_value());
  if (!difference) return {};
  EXPECT_FALSE(difference->in_events);
  EXPECT_NE(difference->what.find(what), std::string::npos) << difference->what;
  const std::vector<Command> prefix(commands.begin(),
                                    commands.begin() + static_cast<std::ptrdiff_t>(difference->index + 1));
  return shrink_to_scenario<ReferenceBook, Broken>(prefix);
}

TEST(Review3Differential, AStaleBestPriceShrinksToAScenarioTheBrokenBookFails) {
  const std::string scenario = shrunk_state_difference<BookWithAStaleBestPrice>(
      parse_lines({"new 1 buy limit 5 @ 100", "new 2 buy limit 5 @ 99", "cancel 1"}), "bids best price: expected 99");
  ASSERT_NE(scenario, "");
  EXPECT_NE(scenario.find("# the events agree, but then bids best price: expected none, actual 100"),
            std::string::npos)
      << scenario;
  EXPECT_TRUE(passes<ReferenceBook>(scenario)) << scenario;
  EXPECT_FALSE(passes<BookWithAStaleBestPrice>(scenario)) << "the broken book passes its own scenario:\n" << scenario;
}

TEST(Review3Differential, AStaleBestPriceOnARandomStreamShrinksToAScenarioTheBrokenBookFails) {
  const std::string scenario = shrunk_state_difference<BookWithAStaleBestPrice>(
      random_commands(3, 2'000, FlowOptions{.target_resting = 50}), "best price: expected ");
  ASSERT_NE(scenario, "");
  EXPECT_TRUE(passes<ReferenceBook>(scenario)) << scenario;
  EXPECT_FALSE(passes<BookWithAStaleBestPrice>(scenario)) << "the broken book passes its own scenario:\n" << scenario;
}

TEST(Review3Differential, AWrongLastAcceptedIdShrinksToAScenarioTheBrokenBookFails) {
  const std::string scenario = shrunk_state_difference<BookThatReportsRejectedIds>(
      parse_lines({"new 1 buy limit 5 @ 100", "new 2 buy limit 0 @ 100"}), "last accepted id: expected 1, actual 2");
  ASSERT_NE(scenario, "");
  EXPECT_TRUE(passes<ReferenceBook>(scenario)) << scenario;
  EXPECT_FALSE(passes<BookThatReportsRejectedIds>(scenario)) << "the broken book passes its own scenario:\n"
                                                              << scenario;
}

TEST(Review3Differential, AWrongSizeShrinksToAScenarioTheBrokenBookFails) {
  const std::string scenario = shrunk_state_difference<BookThatCountsFilledMakers>(
      parse_lines({"new 1 sell limit 5 @ 101", "new 2 sell limit 5 @ 102", "new 3 buy ioc 5 @ 101"}),
      "size: expected 1, actual 2");
  ASSERT_NE(scenario, "");
  EXPECT_TRUE(passes<ReferenceBook>(scenario)) << scenario;
  EXPECT_FALSE(passes<BookThatCountsFilledMakers>(scenario)) << "the broken book passes its own scenario:\n"
                                                              << scenario;
}

}  // namespace
}  // namespace orderbook
