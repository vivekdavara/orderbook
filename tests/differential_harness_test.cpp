// The differential harness itself: it finds a difference wherever one is (in events or only in state), stops at
// the first, shrinks it to the commands it needs, and writes a scenario file that the expected book passes and the
// broken book fails. The books here are the reference model with one deliberate bug each (broken_books.hpp).

#include "orderbook/differential.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "broken_books.hpp"
#include "orderbook/random_flow.hpp"
#include "orderbook/reference_book.hpp"
#include "orderbook/scenario.hpp"
#include "orderbook/text.hpp"

namespace orderbook {
namespace {

std::vector<Command> random_commands(std::uint64_t seed, std::size_t count, FlowOptions options) {
  RandomFlow flow(seed, options);
  std::vector<Command> commands;
  for (std::size_t i = 0; i < count; ++i) commands.push_back(flow.next());
  return commands;
}

std::vector<Command> parse_lines(const std::vector<std::string>& lines) {
  std::vector<Command> commands;
  for (const std::string& line : lines) commands.push_back(*parse_command(line));
  return commands;
}

// Whether a scenario file passes on a fresh book of the given type.
template <typename AnyBook>
bool passes(const std::string& text) {
  const auto scenario = parse_scenario(text);
  EXPECT_TRUE(scenario.has_value()) << text;
  if (!scenario) return false;
  AnyBook book;
  return run_scenario(*scenario, book).empty();
}

TEST(Differential, TwoReferenceModelsNeverDiffer) {
  const std::vector<Command> commands = random_commands(5, 5'000, FlowOptions{.target_resting = 200});
  EXPECT_EQ((first_difference<ReferenceBook, ReferenceBook>(commands, 1)), std::nullopt);
  EXPECT_EQ((shrink_to_scenario<ReferenceBook, ReferenceBook>(commands)), "");
}

TEST(Differential, ReportsTheFirstCommandWhoseEventsDiffer) {
  const std::vector<Command> commands = parse_lines({"new 1 sell limit 10 @ 101", "new 2 buy limit 4 @ 101",
                                                     "new 3 buy limit 4 @ 101", "new 4 buy limit 1 @ 101"});
  const auto difference = first_difference<ReferenceBook, broken::BookWithAPriceBug>(commands);
  ASSERT_TRUE(difference.has_value());
  EXPECT_EQ(difference->index, 2U);  // the third command, the maker's second trade
  EXPECT_EQ(difference->what,
            "command 3: events: expected 'accepted 3 | trade 3 1 4 @ 101', actual 'accepted 3 | trade 3 1 4 @ 102'");
}

TEST(Differential, FindsADifferenceInStateWhenEveryEventAgrees) {
  const std::vector<Command> commands =
      parse_lines({"new 1 buy limit 1 @ 100", "new 2 buy limit 1 @ 99", "new 3 buy limit 1 @ 98", "cancel 3"});
  // The events are all the same, so only a state comparison sees it: at a checkpoint, or at the end.
  const auto at_the_end = first_difference<ReferenceBook, broken::BookWithShortDepth>(commands);
  EXPECT_EQ(at_the_end, std::nullopt);  // the third level was cancelled again before the end
  const auto every_command = first_difference<ReferenceBook, broken::BookWithShortDepth>(commands, 1);
  ASSERT_TRUE(every_command.has_value());
  EXPECT_EQ(every_command->index, 2U);
  EXPECT_EQ(every_command->what, "after command 3: bids depth: expected '100x1 99x1 98x1', actual '100x1 99x1'");
}

TEST(Differential, ComparesStatesAfterEveryNthCommand) {
  // The depth is wrong only after the third command (three levels), and right again after the fourth.
  const std::vector<Command> commands =
      parse_lines({"new 1 buy limit 1 @ 100", "new 2 buy limit 1 @ 99", "new 3 buy limit 1 @ 98", "cancel 3"});
  const auto every_third = first_difference<ReferenceBook, broken::BookWithShortDepth>(commands, 3);
  ASSERT_TRUE(every_third.has_value());
  EXPECT_EQ(every_third->index, 2U);
  EXPECT_EQ((first_difference<ReferenceBook, broken::BookWithShortDepth>(commands, 2)), std::nullopt);  // after 2, 4
}

TEST(Differential, ARandomStreamComparesTheFinalStateEvenWithNoCheckpoints) {
  const FlowOptions options{.target_resting = 50};
  const StreamDifference result =
      diff_random_stream<ReferenceBook, broken::BookWithShortDepth>(3, options, 2'000, 0);
  ASSERT_TRUE(result.difference.has_value());
  EXPECT_FALSE(result.difference->in_events);
  EXPECT_EQ(result.commands, 2'000U);  // the end of the stream, the only state comparison
}

TEST(Differential, ComparesEveryPartOfTheState) {
  // Each book below gets one thing wrong that no event shows, and each is named in the difference.
  const auto best = first_difference<ReferenceBook, broken::BookWithNoBestPriceForALoneOrder>(
      parse_lines({"new 1 buy limit 1 @ 100"}), 1);
  ASSERT_TRUE(best.has_value());
  EXPECT_EQ(best->what, "after command 1: bids best price: expected 100, actual none");

  const auto size = first_difference<ReferenceBook, broken::BookThatCountsCancelledOrders>(
      parse_lines({"new 1 sell limit 1 @ 100", "cancel 1"}), 1);
  ASSERT_TRUE(size.has_value());
  EXPECT_EQ(size->what, "after command 2: size: expected 0, actual 1");

  const auto last_id = first_difference<ReferenceBook, broken::BookThatReportsTheLastIdSeen>(
      parse_lines({"new 5 buy limit 1 @ 100", "new 9 buy limit 0 @ 100"}), 1);
  ASSERT_TRUE(last_id.has_value());
  EXPECT_EQ(last_id->what, "after command 2: last accepted id: expected 5, actual 9");

  // And a side's orders, in priority order: here a queue in the wrong order, whose depth is right.
  const auto orders = first_difference<ReferenceBook, broken::BookWithNewestFirstQueues>(
      parse_lines({"new 1 buy limit 1 @ 100", "new 2 buy limit 2 @ 100"}), 1);
  ASSERT_TRUE(orders.has_value());
  EXPECT_EQ(orders->what, "after command 2: bids: expected '1:100x1 2:100x2', actual '2:100x2 1:100x1'");
}

TEST(Differential, ShrinksARandomPriceBugToAScenarioTheBrokenBookFails) {
  const std::vector<Command> commands = random_commands(12, 3'000, FlowOptions{.target_resting = 100});
  const auto difference = first_difference<ReferenceBook, broken::BookWithAPriceBug>(commands);
  ASSERT_TRUE(difference.has_value());
  const std::vector<Command> prefix(commands.begin(),
                                    commands.begin() + static_cast<std::ptrdiff_t>(difference->index + 1));
  const std::string scenario = shrink_to_scenario<ReferenceBook, broken::BookWithAPriceBug>(prefix);
  ASSERT_NE(scenario, "");
  // A resting order, a taker that fills part of it, and a second taker: three commands, whatever the stream.
  EXPECT_NE(scenario.find("# The books disagree on these 3 commands, shrunk from " +
                          std::to_string(prefix.size()) + ".\n"),
            std::string::npos)
      << scenario;
  EXPECT_NE(scenario.find("# but the book under test gave: "), std::string::npos) << scenario;
  EXPECT_TRUE(passes<ReferenceBook>(scenario)) << scenario;
  EXPECT_FALSE(passes<broken::BookWithAPriceBug>(scenario)) << scenario;
}

TEST(Differential, ShrinksAStateOnlyBugToAScenarioWhoseDepthCatchesIt) {
  const std::vector<Command> commands = random_commands(3, 2'000, FlowOptions{.target_resting = 50});
  const auto difference = first_difference<ReferenceBook, broken::BookWithShortDepth>(commands, 1);
  ASSERT_TRUE(difference.has_value());
  const std::vector<Command> prefix(commands.begin(),
                                    commands.begin() + static_cast<std::ptrdiff_t>(difference->index + 1));
  const std::string scenario = shrink_to_scenario<ReferenceBook, broken::BookWithShortDepth>(prefix);
  // Three orders resting at three prices on one side; nothing else is needed.
  EXPECT_NE(scenario.find("# The books disagree on these 3 commands"), std::string::npos) << scenario;
  EXPECT_NE(scenario.find("# the events agree, but then "), std::string::npos) << scenario;
  EXPECT_TRUE(passes<ReferenceBook>(scenario)) << scenario;
  EXPECT_FALSE(passes<broken::BookWithShortDepth>(scenario)) << scenario;
}

TEST(Differential, ABugTheStateShowsFirstStillShrinksToEventsThatShowIt) {
  // With time priority backwards, the states differ as soon as two orders rest at one price, and the events only
  // once a taker reaches them. Shrunk by the state alone, the stream would be two resting orders whose events agree;
  // the scenario file must instead show a trade with the wrong maker, so that the broken book fails it.
  const std::vector<Command> commands = random_commands(1, 2'000, FlowOptions{.target_resting = 50});
  const auto by_state = first_difference<ReferenceBook, broken::BookWithNewestFirstQueues>(commands, 1);
  const auto by_events = first_difference<ReferenceBook, broken::BookWithNewestFirstQueues>(commands);
  ASSERT_TRUE(by_state.has_value() && by_events.has_value());
  EXPECT_FALSE(by_state->in_events);
  EXPECT_TRUE(by_events->in_events);
  EXPECT_LT(by_state->index, by_events->index);
  const std::vector<Command> prefix(commands.begin(),
                                    commands.begin() + static_cast<std::ptrdiff_t>(by_events->index + 1));
  const std::string scenario = shrink_to_scenario<ReferenceBook, broken::BookWithNewestFirstQueues>(prefix);
  // Two orders at one price, and a taker that trades with one of them.
  EXPECT_NE(scenario.find("# The books disagree on these 3 commands"), std::string::npos) << scenario;
  EXPECT_NE(scenario.find("# but the book under test gave: "), std::string::npos) << scenario;
  EXPECT_TRUE(passes<ReferenceBook>(scenario)) << scenario;
  EXPECT_FALSE(passes<broken::BookWithNewestFirstQueues>(scenario)) << scenario;
}

TEST(Differential, ARandomStreamRunsToItsEndWhenTheBooksAgree) {
  const StreamDifference result =
      diff_random_stream<ReferenceBook, ReferenceBook>(6, FlowOptions{.target_resting = 100}, 10'000, 100);
  EXPECT_EQ(result.commands, 10'000U);
  EXPECT_FALSE(result.difference.has_value());
  EXPECT_EQ((shrink_random_stream<ReferenceBook, ReferenceBook>(6, FlowOptions{.target_resting = 100}, 10'000)), "");
}

TEST(Differential, ARandomStreamStopsAtTheFirstDifferenceAndShrinksIt) {
  const FlowOptions options{.target_resting = 100};
  const StreamDifference result = diff_random_stream<ReferenceBook, broken::BookWithAPriceBug>(12, options, 3'000, 100);
  ASSERT_TRUE(result.difference.has_value());
  // It stopped where first_difference stops on the same commands, made again from the seed.
  const std::vector<Command> commands = random_commands(12, 3'000, options);
  const auto expected = first_difference<ReferenceBook, broken::BookWithAPriceBug>(commands, 100);
  ASSERT_TRUE(expected.has_value());
  EXPECT_EQ(result.difference->index, expected->index);
  EXPECT_EQ(result.difference->what, expected->what);
  EXPECT_EQ(result.commands, expected->index + 1);
  const std::string scenario =
      shrink_random_stream<ReferenceBook, broken::BookWithAPriceBug>(12, options, result.commands);
  EXPECT_NE(scenario.find("# The books disagree on these 3 commands, shrunk from " + std::to_string(result.commands) +
                          ".\n"),
            std::string::npos)
      << scenario;
  EXPECT_FALSE(passes<broken::BookWithAPriceBug>(scenario)) << scenario;
}

TEST(Differential, ARandomStreamFindsAStateDifferenceAtACheckpoint) {
  const StreamDifference result =
      diff_random_stream<ReferenceBook, broken::BookWithShortDepth>(3, FlowOptions{.target_resting = 50}, 2'000, 10);
  ASSERT_TRUE(result.difference.has_value());
  EXPECT_FALSE(result.difference->in_events);
  EXPECT_EQ(result.commands % 10, 0U);  // found at a checkpoint
  EXPECT_NE(result.difference->what.find("after command "), std::string::npos) << result.difference->what;
  const std::string scenario = shrink_random_stream<ReferenceBook, broken::BookWithShortDepth>(
      3, FlowOptions{.target_resting = 50}, result.commands);
  EXPECT_FALSE(passes<broken::BookWithShortDepth>(scenario)) << scenario;
}

TEST(Differential, WritesTheExpectedBooksEventsDepthAndState) {
  const std::vector<Command> commands =
      parse_lines({"new 1 sell limit 10 @ 101", "new 2 buy limit 4 @ 101", "new 3 buy ioc 7 @ 101"});
  EXPECT_EQ((differential_scenario<ReferenceBook, broken::BookWithAPriceBug>(commands, "# heading\n")),
            "# heading\n"
            "> new 1 sell limit 10 @ 101\n"
            "accepted 1\n"
            "> new 2 buy limit 4 @ 101\n"
            "accepted 2\n"
            "trade 2 1 4 @ 101\n"
            "> new 3 buy ioc 7 @ 101\n"
            "accepted 3\n"
            "trade 3 1 6 @ 101\n"
            "cancelled 3 1 unfilled\n"
            "# but the book under test gave: accepted 3 | trade 3 1 6 @ 102 | cancelled 3 1 unfilled\n"
            "= bids\n"
            "= asks\n"
            "= orders bids\n"
            "= orders asks\n"
            "= best bids none\n"
            "= best asks none\n"
            "= resting 0\n"
            "= last-accepted 3\n");
}

}  // namespace
}  // namespace orderbook
