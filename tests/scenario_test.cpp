#include "orderbook/scenario.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "orderbook/book.hpp"
#include "orderbook/reference_book.hpp"

namespace orderbook {
namespace {

namespace fs = std::filesystem;

// The scenario files' names, sorted.
std::vector<std::string> scenario_files() {
  std::vector<std::string> files;
  // A missing directory gives no files (and TheFilesAreThere fails) rather than an exception while tests register.
  std::error_code error;
  for (fs::directory_iterator it(ORDERBOOK_SCENARIO_DIR, error), end; !error && it != end; it.increment(error)) {
    if (it->path().extension() == ".txt") files.push_back(it->path().filename().string());
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

class ScenarioFile : public ::testing::TestWithParam<std::string> {};

// Every file runs on both books: it is the specification, written from DESIGN.md, so each must pass it.
TEST_P(ScenarioFile, PassesOnTheReferenceModel) {
  std::string error;
  const auto scenario = parse_scenario(read_file(fs::path(ORDERBOOK_SCENARIO_DIR) / GetParam()), &error);
  ASSERT_TRUE(scenario.has_value()) << GetParam() << " " << error;
  ASSERT_FALSE(scenario->empty()) << GetParam() << " has no commands";
  ReferenceBook book;
  EXPECT_EQ(run_scenario(*scenario, book), "") << GetParam();
}

TEST_P(ScenarioFile, PassesOnTheEngine) {
  std::string error;
  const auto scenario = parse_scenario(read_file(fs::path(ORDERBOOK_SCENARIO_DIR) / GetParam()), &error);
  ASSERT_TRUE(scenario.has_value()) << GetParam() << " " << error;
  ASSERT_FALSE(scenario->empty()) << GetParam() << " has no commands";
  Book book;
  EXPECT_EQ(run_scenario(*scenario, book), "") << GetParam();
}

// "03_time_priority.txt" runs as Files/ScenarioFile.PassesOnTheReferenceModel/03_time_priority.
std::string test_name(const ::testing::TestParamInfo<std::string>& param) {
  std::string name = fs::path(param.param).stem().string();
  std::replace_if(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) == 0; },
                  '_');
  return name;
}

INSTANTIATE_TEST_SUITE_P(Files, ScenarioFile, ::testing::ValuesIn(scenario_files()), test_name);

TEST(Scenario, TheFilesAreThere) {
  // A wrong directory would otherwise instantiate no file tests, and pass.
  EXPECT_GE(scenario_files().size(), 15U) << ORDERBOOK_SCENARIO_DIR;
}

TEST(Scenario, ParsesCommandsEventsAndDepthWithTheirLines) {
  const auto scenario = parse_scenario(
      "# a comment\n"
      "\n"
      "> new 1 sell limit 10 @ 101\n"
      "accepted   1\n"
      "= asks 101x10\n"
      "= bids\n"
      ">cancel 1\r\n"
      "\tcancelled 1 10 requested\n");
  ASSERT_TRUE(scenario.has_value());
  ASSERT_EQ(scenario->size(), 2U);
  const ScenarioStep& first = (*scenario)[0];
  EXPECT_EQ(first.line, 3);
  EXPECT_EQ(first.command, (Command{NewOrder{OrderId{1}, Side::Sell, OrderType::Limit, Quantity{10}, Price{101}}}));
  EXPECT_EQ(first.events, std::vector<std::string>{"accepted 1"});  // spacing normalised
  ASSERT_EQ(first.depth.size(), 2U);
  EXPECT_EQ(first.depth[0].line, 5);
  EXPECT_EQ(first.depth[0].side, Side::Sell);
  EXPECT_EQ(first.depth[0].levels, (std::vector<Level>{{Price{101}, Quantity{10}}}));
  EXPECT_EQ(first.depth[1].side, Side::Buy);
  EXPECT_TRUE(first.depth[1].levels.empty());
  const ScenarioStep& second = (*scenario)[1];
  EXPECT_EQ(second.line, 7);
  EXPECT_EQ(second.command, Command{CancelOrder{OrderId{1}}});
  EXPECT_EQ(second.events, std::vector<std::string>{"cancelled 1 10 requested"});
}

TEST(Scenario, AnEmptyFileHasNoSteps) {
  EXPECT_EQ(parse_scenario(""), Scenario{});
  EXPECT_EQ(parse_scenario("# only a comment\n\n"), Scenario{});
}

struct BadScenario {
  const char* text;
  const char* error;
};

TEST(Scenario, RejectsMalformedFilesNamingTheLine) {
  const std::vector<BadScenario> cases{
      {"accepted 1\n", "line 1: an expectation before any command"},
      {"# x\n= bids\n", "line 2: an expectation before any command"},
      {"> new 1 buy limit 10\n", "line 1: limit orders need '@ <price>'"},
      {"> bogus\n", "line 1: unknown command 'bogus'"},
      {"> new 1 buy limit 1 @ 1\n= buys 1x1\n", "line 2: expected '= bids ...', '= asks ...', '= best ...'"},
      {"> new 1 buy limit 1 @ 1\n=\n", "line 2: expected '= bids ...', '= asks ...', '= best ...'"},
      {"> new 1 buy limit 1 @ 1\n= best buys 1\n", "line 2: expected '= best bids ...' or '= best asks ...'"},
      {"> new 1 buy limit 1 @ 1\n= best bids\n", "line 2: bad best price '', expected a price or 'none'"},
      {"> new 1 buy limit 1 @ 1\n= best bids 1.5\n", "line 2: bad best price '1.5'"},
      {"> new 1 buy limit 1 @ 1\n= orders asks 1:100\n", "line 2: bad order '1:100', expected <id>:<price>x"},
      {"> new 1 buy limit 1 @ 1\n= orders asks 100x1\n", "line 2: bad order '100x1'"},
      {"> new 1 buy limit 1 @ 1\n= orders bids -1:100x1\n", "line 2: bad order '-1:100x1'"},
      {"> new 1 buy limit 1 @ 1\n= resting\n", "line 2: bad number '' for '= resting'"},
      {"> new 1 buy limit 1 @ 1\n= last-accepted -1\n", "line 2: bad number '-1' for '= last-accepted'"},
      {"> new 1 buy limit 1 @ 1\n= resting 1\naccepted 1\n", "line 3: an event after the step's depth or state"},
      {"> new 1 buy limit 1 @ 1\n= bids 1x\n", "line 2: bad level '1x'"},
      {"> new 1 buy limit 1 @ 1\n= bids 1x1\naccepted 1\n", "line 3: an event after the step's depth or state"},
  };
  for (const BadScenario& c : cases) {
    std::string error;
    EXPECT_EQ(parse_scenario(c.text, &error), std::nullopt) << c.text;
    EXPECT_NE(error.find(c.error), std::string::npos) << "'" << c.text << "' gave '" << error << "'";
  }
}

TEST(Scenario, ReportsTheFirstWrongEventsWithTheirLine) {
  const auto scenario = parse_scenario(
      "> new 1 sell limit 10 @ 101\n"
      "accepted 1\n"
      "> new 2 buy limit 4 @ 101\n"
      "accepted 2\n"
      "trade 2 1 4 @ 100\n"
      "> new 3 buy limit 1 @ 1\n"
      "nonsense\n");
  ASSERT_TRUE(scenario.has_value());
  ReferenceBook book;
  EXPECT_EQ(run_scenario(*scenario, book),
            "line 3: 'new 2 buy limit 4 @ 101'\n"
            "expected events:\n  accepted 2\n  trade 2 1 4 @ 100\n"
            "actual events:\n  accepted 2\n  trade 2 1 4 @ 101\n");
  // It stopped there: the third command never ran.
  EXPECT_EQ(book.last_accepted_id(), OrderId{2});
}

TEST(Scenario, MissingAndExtraEventsAreMismatchesToo) {
  ReferenceBook book;
  EXPECT_NE(run_scenario(*parse_scenario("> new 1 buy ioc 1 @ 1\naccepted 1\n"), book), "");
  ReferenceBook other;
  EXPECT_NE(run_scenario(*parse_scenario("> new 1 buy limit 1 @ 1\naccepted 1\naccepted 1\n"), other), "");
  ReferenceBook third;
  EXPECT_NE(run_scenario(*parse_scenario("> new 1 buy limit 1 @ 1\n"), third), "");
}

TEST(Scenario, ParsesStateChecks) {
  const auto scenario = parse_scenario(
      "> new 3 sell limit 5 @ 101\n"
      "accepted 3\n"
      "= best asks 101\n"
      "=  best   bids   none\n"
      "= orders asks 3:101x5\n"
      "= orders bids\n"
      "= resting 1\n"
      "= last-accepted 3\n");
  ASSERT_TRUE(scenario.has_value());
  const std::vector<StateCheck>& state = (*scenario)[0].state;
  ASSERT_EQ(state.size(), 6U);
  EXPECT_EQ(state[0], (StateCheck{3, StateCheck::Kind::BestPrice, Side::Sell, Price{101}, {}, 0}));
  EXPECT_EQ(state[1], (StateCheck{4, StateCheck::Kind::BestPrice, Side::Buy, std::nullopt, {}, 0}));
  EXPECT_EQ(state[2], (StateCheck{5, StateCheck::Kind::Orders, Side::Sell, std::nullopt,
                                  {{OrderId{3}, Side::Sell, Price{101}, Quantity{5}}}, 0}));
  EXPECT_EQ(state[3], (StateCheck{6, StateCheck::Kind::Orders, Side::Buy, std::nullopt, {}, 0}));
  EXPECT_EQ(state[4], (StateCheck{7, StateCheck::Kind::Resting, Side::Buy, std::nullopt, {}, 1}));
  EXPECT_EQ(state[5], (StateCheck{8, StateCheck::Kind::LastAccepted, Side::Buy, std::nullopt, {}, 3}));
  // Each formats back to its line, spacing normalised.
  EXPECT_EQ(format(state[0]), "= best asks 101");
  EXPECT_EQ(format(state[1]), "= best bids none");
  EXPECT_EQ(format(state[2]), "= orders asks 3:101x5");
  EXPECT_EQ(format(state[3]), "= orders bids");
  EXPECT_EQ(format(state[4]), "= resting 1");
  EXPECT_EQ(format(state[5]), "= last-accepted 3");
  ReferenceBook book;
  EXPECT_EQ(run_scenario(*scenario, book), "");
  Book engine;
  EXPECT_EQ(run_scenario(*scenario, engine), "");
}

TEST(Scenario, ReportsAWrongStateWithItsLine) {
  const std::string start =
      "> new 1 buy limit 5 @ 100\n"
      "accepted 1\n"
      "> new 2 buy limit 3 @ 100\n"
      "accepted 2\n";
  const std::vector<std::pair<std::string, std::string>> cases{
      {"= best bids 99\n", "line 5: expected '= best bids 99', actual '= best bids 100'"},
      {"= best asks 100\n", "line 5: expected '= best asks 100', actual '= best asks none'"},
      {"= orders bids 2:100x3 1:100x5\n", "line 5: expected '= orders bids 2:100x3 1:100x5', actual '= orders bids "
                                          "1:100x5 2:100x3'"},
      {"= orders asks 1:100x5\n", "line 5: expected '= orders asks 1:100x5', actual '= orders asks'"},
      {"= resting 1\n", "line 5: expected '= resting 1', actual '= resting 2'"},
      {"= last-accepted 1\n", "line 5: expected '= last-accepted 1', actual '= last-accepted 2'"},
  };
  for (const auto& [check, report] : cases) {
    const auto scenario = parse_scenario(start + check);
    ASSERT_TRUE(scenario.has_value()) << check;
    ReferenceBook book;
    EXPECT_EQ(run_scenario(*scenario, book), report) << check;
    Book engine;
    EXPECT_EQ(run_scenario(*scenario, engine), report) << check;
  }
}

// The reference model, except that it reports its bids as sells: lists that print the same and still differ.
class BookWithSellsForBids {
 public:
  void apply(const Command& command, std::vector<Event>& events) { book_.apply(command, events); }
  std::vector<Level> depth(Side side) const { return book_.depth(side); }
  std::optional<Price> best_price(Side side) const { return book_.best_price(side); }
  std::vector<RestingOrder> orders(Side side) const {
    std::vector<RestingOrder> reported = book_.orders(side);
    for (RestingOrder& order : reported) order.side = Side::Sell;
    return reported;
  }
  std::size_t size() const { return book_.size(); }
  OrderId last_accepted_id() const { return book_.last_accepted_id(); }

 private:
  ReferenceBook book_;
};

TEST(Scenario, ReportsOrdersThatPrintTheSameWithTheirSides) {
  const auto scenario = parse_scenario("> new 1 buy limit 5 @ 100\naccepted 1\n= orders bids 1:100x5\n");
  ASSERT_TRUE(scenario.has_value());
  BookWithSellsForBids book;
  EXPECT_EQ(run_scenario(*scenario, book),
            "line 3: expected '= orders bids 1:100x5(buy)', actual '= orders bids 1:100x5(sell)'");
}

TEST(Scenario, ReportsAWrongDepthWithItsLine) {
  const auto scenario = parse_scenario(
      "> new 1 sell limit 10 @ 101\n"
      "accepted 1\n"
      "= bids\n"
      "= asks 101x9\n");
  ASSERT_TRUE(scenario.has_value());
  ReferenceBook book;
  EXPECT_EQ(run_scenario(*scenario, book), "line 4: expected '= asks 101x9', actual '= asks 101x10'");
  ReferenceBook other;
  EXPECT_EQ(run_scenario(*parse_scenario("> new 1 sell limit 10 @ 101\naccepted 1\n= asks\n"), other),
            "line 3: expected '= asks', actual '= asks 101x10'");
}

}  // namespace
}  // namespace orderbook
