// Review 4. A resting order's `side` is part of what the state checks compare (RestingOrder's operator==, used by
// state_difference and by run_scenario through observe()), but no text says it: format(vector<RestingOrder>) and
// describe(vector<RestingOrder>) print only id:price x remaining, and parse_orders takes the side from the line.
// So a book that reports an order with the wrong side is caught, and then reported as two identical strings:
//
//   line 3: expected '= orders bids 1:100x5', actual '= orders bids 1:100x5'
//   after command 1: bids: expected '1:100x5', actual '1:100x5'
//
// DESIGN.md ("The text format") says the runner "reports what was expected, what happened, and the line"; here what
// happened is indistinguishable from what was expected. Each test below asks only that a reported mismatch show a
// difference between its two halves.

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../broken_books.hpp"
#include "orderbook/differential.hpp"
#include "orderbook/reference_book.hpp"
#include "orderbook/scenario.hpp"
#include "orderbook/text.hpp"

namespace orderbook {
namespace {

// The reference model, except that orders(side) reports each order with the other side in its `side` field.
class BookWithTheWrongSideOnItsOrders : public broken::WrappedReferenceBook {
 public:
  void apply(const Command& command, std::vector<Event>& events) { book_.apply(command, events); }
  std::vector<RestingOrder> orders(Side side) const {
    std::vector<RestingOrder> reported = book_.orders(side);
    for (RestingOrder& order : reported) order.side = opposite(order.side);
    return reported;
  }
};

// "X', actual 'Y'" -> {X, Y}, from a report of the form "...expected 'X', actual 'Y'".
std::pair<std::string, std::string> halves(const std::string& report) {
  const std::size_t expected = report.find("expected '");
  const std::size_t actual = report.find("', actual '");
  if (expected == std::string::npos || actual == std::string::npos) return {report, ""};
  const std::size_t start = expected + std::string("expected '").size();
  const std::size_t actual_start = actual + std::string("', actual '").size();
  return {report.substr(start, actual - start), report.substr(actual_start, report.rfind('\'') - actual_start)};
}

TEST(Review4StateReport, AnOrderOnTheWrongSideIsReportedAsADifferenceTheReaderCanSee) {
  const auto scenario = parse_scenario(
      "> new 1 buy limit 5 @ 100\n"
      "accepted 1\n"
      "= orders bids 1:100x5\n");
  ASSERT_TRUE(scenario.has_value());
  ReferenceBook reference;
  EXPECT_EQ(run_scenario(*scenario, reference), "");
  BookWithTheWrongSideOnItsOrders broken_book;
  const std::string report = run_scenario(*scenario, broken_book);
  ASSERT_NE(report, "") << "the wrong side went unnoticed";
  const auto [expected, actual] = halves(report);
  EXPECT_NE(expected, actual) << "the report shows no difference: " << report;
}

TEST(Review4StateReport, TheHarnessNamesWhatDiffersInAnOrderOnTheWrongSide) {
  const std::vector<Command> commands{*parse_command("new 1 buy limit 5 @ 100")};
  const auto difference = first_difference<ReferenceBook, BookWithTheWrongSideOnItsOrders>(commands, 1);
  ASSERT_TRUE(difference.has_value());
  const auto [expected, actual] = halves(difference->what);
  EXPECT_NE(expected, actual) << "the difference reads as no difference: " << difference->what;
}

}  // namespace
}  // namespace orderbook
