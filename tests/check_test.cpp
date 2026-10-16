#include "orderbook/check.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "orderbook/random_flow.hpp"
#include "orderbook/reference_book.hpp"
#include "orderbook/text.hpp"

namespace orderbook {
namespace {

std::string joined(const std::vector<std::string>& lines) {
  std::string text;
  for (const std::string& line : lines) text += line + "\n";
  return text;
}

struct RunOptions {
  std::uint64_t seed;
  FlowOptions flow;
  int commands;
};

// Runs a random stream through the reference model with every check on: the audit after every command, the state
// checks every 25 commands, and the audit against the book every 250 and at the end.
void run_checked(const RunOptions& run) {
  RandomFlow flow(run.seed, run.flow);
  ReferenceBook book;
  Audit audit;
  std::vector<Event> events;
  for (int i = 1; i <= run.commands; ++i) {
    const Command command = flow.next();
    events.clear();
    book.apply(command, events);
    audit.record(command, events);
    ASSERT_TRUE(audit.ok()) << "seed " << run.seed << "\n" << joined(audit.problems());
    if (i % 25 == 0) ASSERT_EQ(joined(check_book(book)), "") << "seed " << run.seed << ", command " << i;
    if (i % 250 == 0) audit.compare(book);
  }
  audit.compare(book);
  EXPECT_TRUE(audit.ok()) << joined(audit.problems());
  EXPECT_EQ(audit.resting(), book.size());
}

TEST(Invariants, HoldOnLongRandomRuns) {
  for (std::uint64_t seed = 1; seed <= 4; ++seed) run_checked({seed, FlowOptions{.target_resting = 300}, 25'000});
}

TEST(Invariants, HoldWhenFewPricesMakeLongQueues) {
  // Three ticks of range: most orders queue at a handful of prices, so time priority decides nearly every trade.
  run_checked({21, FlowOptions{.price_range = 3, .max_quantity = 20, .target_resting = 200}, 25'000});
}

TEST(Invariants, HoldWithLargeQuantitiesAndANearlyEmptyBook) {
  // Few resting orders, so market and IOC orders often find an empty or thin side.
  run_checked({33, FlowOptions{.max_quantity = 1'000'000'000, .target_resting = 5}, 25'000});
}

TEST(Invariants, HoldWithManyInvalidOrders) {
  run_checked({44, FlowOptions{.target_resting = 100, .invalid_per_mille = 300}, 10'000});
}

// The checks have to be able to fail: each case below feeds the audit events that break one rule.

Command command(std::string_view line) { return *parse_command(line); }

// An audit that has seen the given commands answered by the reference model.
Audit audit_after(const std::vector<std::string_view>& lines) {
  ReferenceBook book;
  Audit audit;
  std::vector<Event> events;
  for (const std::string_view line : lines) {
    events.clear();
    book.apply(command(line), events);
    audit.record(command(line), events);
  }
  EXPECT_TRUE(audit.ok()) << joined(audit.problems());
  return audit;
}

struct WrongEvents {
  const char* rule;
  std::vector<Event> events;
  const char* problem;
};

TEST(Audit, CatchesEachBrokenRule) {
  // Resting: asks 1 (5 @ 101), 2 (5 @ 101, later), 3 (5 @ 102).
  const std::vector<std::string_view> setup{"new 1 sell limit 5 @ 101", "new 2 sell limit 5 @ 101",
                                            "new 3 sell limit 5 @ 102"};
  const OrderId taker{4};
  const std::vector<WrongEvents> cases{
      {"time priority", {Accepted{taker}, Trade{taker, OrderId{2}, Quantity{5}, Price{101}}},
       "the best resting order was 1"},
      {"price priority", {Accepted{taker}, Trade{taker, OrderId{3}, Quantity{5}, Price{102}}},
       "the best resting order was 1"},
      {"the maker's price", {Accepted{taker}, Trade{taker, OrderId{1}, Quantity{5}, Price{102}}},
       "the maker's price is 101"},
      {"the fill size", {Accepted{taker}, Trade{taker, OrderId{1}, Quantity{4}, Price{101}}},
       "the smaller remaining quantity is 5"},
      {"the taker", {Accepted{taker}, Trade{OrderId{9}, OrderId{1}, Quantity{5}, Price{101}}},
       "the taker isn't the new order"},
      {"stopping early", {Accepted{taker}}, "still crossing order 1"},
      {"a limit order rests silently",
       {Accepted{taker}, Trade{taker, OrderId{1}, Quantity{5}, Price{101}},
        Trade{taker, OrderId{2}, Quantity{1}, Price{101}}, Cancelled{taker, Quantity{0}, CancelReason::Unfilled}},
       "after its trades expected '(nothing)'"},
      {"acceptance first", {Trade{taker, OrderId{1}, Quantity{5}, Price{101}}}, "expected 'accepted 4' first"},
      {"validation", {Rejected{taker, RejectReason::InvalidPrice}}, "expected 'accepted 4' first"},
  };
  for (const WrongEvents& c : cases) {
    Audit audit = audit_after(setup);
    // The right answer is "accepted 4", then trades with 1 (5 @ 101) and 2 (1 @ 101).
    audit.record(command("new 4 buy limit 6 @ 101"), c.events);
    ASSERT_FALSE(audit.ok()) << c.rule;
    EXPECT_NE(audit.problems().front().find(c.problem), std::string::npos)
        << c.rule << ": " << audit.problems().front();
  }
}

TEST(Audit, CatchesAnIocThatIsNotCancelledOrAMarketOrderThatRests) {
  Audit audit = audit_after({"new 1 sell limit 5 @ 101"});
  audit.record(command("new 2 buy ioc 8 @ 101"), {Accepted{OrderId{2}}, Trade{OrderId{2}, OrderId{1}, Quantity{5}, Price{101}}});
  ASSERT_FALSE(audit.ok());
  EXPECT_NE(audit.problems().front().find("expected 'cancelled 2 3 unfilled'"), std::string::npos)
      << audit.problems().front();

  Audit other = audit_after({});
  other.record(command("new 1 buy market 5"), {Accepted{OrderId{1}}, Cancelled{OrderId{1}, Quantity{4}, CancelReason::Unfilled}});
  ASSERT_FALSE(other.ok());
  EXPECT_NE(other.problems().front().find("expected 'cancelled 1 5 unfilled'"), std::string::npos)
      << other.problems().front();
}

TEST(Audit, CatchesWrongRejectionsAndCancels) {
  Audit audit = audit_after({"new 5 buy limit 3 @ 100"});
  audit.record(command("new 5 buy limit 0 @ 100"), {Rejected{OrderId{5}, RejectReason::InvalidQuantity}});
  ASSERT_FALSE(audit.ok());  // the id is checked first
  EXPECT_NE(audit.problems().front().find("expected 'rejected 5 id-not-increasing'"), std::string::npos);

  Audit cancels = audit_after({"new 5 buy limit 3 @ 100"});
  cancels.record(command("cancel 5"), {Cancelled{OrderId{5}, Quantity{2}, CancelReason::Requested}});
  ASSERT_FALSE(cancels.ok());
  EXPECT_NE(cancels.problems().front().find("expected 'cancelled 5 3 requested'"), std::string::npos);

  Audit unknown = audit_after({});
  unknown.record(command("cancel 7"), {Cancelled{OrderId{7}, Quantity{1}, CancelReason::Requested}});
  ASSERT_FALSE(unknown.ok());
  EXPECT_NE(unknown.problems().front().find("expected 'rejected 7 unknown-order'"), std::string::npos);
}

// A book whose queries return whatever a test sets, to show check_book and compare() catch a bad state.
struct FakeBook {
  std::vector<RestingOrder> bids, asks;
  std::optional<std::vector<Level>> bid_depth;  // when set, overrides the depth computed from the orders

  std::vector<RestingOrder> orders(Side side) const { return side == Side::Buy ? bids : asks; }
  std::vector<Level> depth(Side side) const {
    if (side == Side::Buy && bid_depth) return *bid_depth;
    std::vector<Level> levels;
    for (const RestingOrder& order : orders(side)) {
      if (levels.empty() || levels.back().price != order.price) levels.push_back({order.price, Quantity{0}});
      levels.back().quantity += order.remaining;
    }
    return levels;
  }
  std::optional<Price> best_price(Side side) const {
    const auto levels = depth(side);
    return levels.empty() ? std::nullopt : std::optional<Price>(levels.front().price);
  }
};

RestingOrder bid(std::uint64_t id, std::int64_t price, std::int64_t remaining) {
  return {OrderId{id}, Side::Buy, Price{price}, Quantity{remaining}};
}
RestingOrder ask(std::uint64_t id, std::int64_t price, std::int64_t remaining) {
  return {OrderId{id}, Side::Sell, Price{price}, Quantity{remaining}};
}

TEST(CheckBook, PassesASoundBook) {
  const FakeBook book{{bid(1, 100, 5), bid(2, 100, 1), bid(3, 99, 7)}, {ask(4, 101, 2)}, std::nullopt};
  EXPECT_EQ(joined(check_book(book)), "");
}

TEST(CheckBook, CatchesEachKindOfBadState) {
  struct Case {
    FakeBook book;
    const char* problem;
  };
  const std::vector<Case> cases{
      {{{bid(1, 101, 5)}, {ask(2, 101, 5)}, std::nullopt}, "crossed: best bid 101, best ask 101"},
      {{{bid(1, 102, 5)}, {ask(2, 101, 5)}, std::nullopt}, "crossed: best bid 102, best ask 101"},
      {{{bid(1, 99, 5), bid(2, 100, 5)}, {}, std::nullopt}, "bids order 2 is out of price priority"},
      {{{}, {ask(1, 102, 5), ask(2, 101, 5)}, std::nullopt}, "asks order 2 is out of price priority"},
      {{{bid(1, 100, 0)}, {}, std::nullopt}, "bids order 1 has 0 lots left"},
      {{{bid(1, 0, 5)}, {}, std::nullopt}, "bids order 1 has an invalid price"},
      {{{bid(1, 100, 5)}, {ask(1, 101, 5)}, std::nullopt}, "asks order 1 rests twice"},
      {{{ask(1, 100, 5)}, {}, std::nullopt}, "bids order 1 is on the wrong side"},
      {{{bid(1, 100, 5)}, {}, std::vector<Level>{{Price{100}, Quantity{6}}}}, "bids depth is '100x6'"},
  };
  for (const Case& c : cases) {
    const std::string problems = joined(check_book(c.book));
    EXPECT_NE(problems.find(c.problem), std::string::npos) << "expected '" << c.problem << "' in:\n" << problems;
  }
}

TEST(Audit, CompareCatchesABookThatDisagreesWithTheEvents) {
  Audit audit = audit_after({"new 1 buy limit 5 @ 100", "new 2 buy limit 3 @ 100", "new 3 sell limit 4 @ 103"});
  const FakeBook right{{bid(1, 100, 5), bid(2, 100, 3)}, {ask(3, 103, 4)}, std::nullopt};
  audit.compare(right);
  EXPECT_TRUE(audit.ok()) << joined(audit.problems());

  // The same orders, but at one price in the wrong arrival order.
  Audit swapped = audit;
  swapped.compare(FakeBook{{bid(2, 100, 3), bid(1, 100, 5)}, {ask(3, 103, 4)}, std::nullopt});
  ASSERT_FALSE(swapped.ok());
  EXPECT_NE(swapped.problems().front().find("bids are '2:100x3 1:100x5', expected '1:100x5 2:100x3'"),
            std::string::npos)
      << swapped.problems().front();

  Audit missing = audit;
  missing.compare(FakeBook{{bid(1, 100, 5), bid(2, 100, 3)}, {}, std::nullopt});
  EXPECT_FALSE(missing.ok());
}

}  // namespace
}  // namespace orderbook
