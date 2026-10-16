#include "orderbook/reference_book.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "orderbook/text.hpp"

namespace orderbook {
namespace {

using Lines = std::vector<std::string>;

// Applies one command written in the text format and returns its events in the text format.
Lines run(ReferenceBook& book, std::string_view line) {
  std::string error;
  const auto command = parse_command(line, &error);
  EXPECT_TRUE(command.has_value()) << line << ": " << error;
  if (!command) return {};
  std::vector<Event> events;
  book.apply(*command, events);
  Lines out;
  for (const Event& event : events) out.push_back(format(event));
  return out;
}

void run_all(ReferenceBook& book, const std::vector<std::string_view>& lines) {
  for (const std::string_view line : lines) run(book, line);
}

TEST(ReferenceBook, StartsEmpty) {
  const ReferenceBook book;
  EXPECT_EQ(book.size(), 0U);
  EXPECT_EQ(book.best_price(Side::Buy), std::nullopt);
  EXPECT_EQ(book.best_price(Side::Sell), std::nullopt);
  EXPECT_TRUE(book.depth(Side::Buy).empty());
  EXPECT_EQ(book.last_accepted_id(), OrderId{0});
}

TEST(ReferenceBook, RestsALimitOrderThatDoesNotCross) {
  ReferenceBook book;
  EXPECT_EQ(run(book, "new 1 buy limit 10 @ 100"), Lines{"accepted 1"});
  EXPECT_EQ(run(book, "new 2 sell limit 4 @ 101"), Lines{"accepted 2"});
  EXPECT_EQ(book.size(), 2U);
  EXPECT_EQ(book.best_price(Side::Buy), Price{100});
  EXPECT_EQ(book.best_price(Side::Sell), Price{101});
  EXPECT_EQ(book.orders(Side::Buy), (std::vector<RestingOrder>{{OrderId{1}, Side::Buy, Price{100}, Quantity{10}}}));
  EXPECT_EQ(book.last_accepted_id(), OrderId{2});
}

TEST(ReferenceBook, ReportsOrdersInPriorityOrderAndDepthBestFirst) {
  ReferenceBook book;
  run_all(book, {"new 1 buy limit 5 @ 99", "new 2 buy limit 3 @ 100", "new 3 buy limit 7 @ 99",
                 "new 4 sell limit 2 @ 103", "new 5 sell limit 6 @ 102", "new 6 sell limit 1 @ 103"});
  const std::vector<RestingOrder> bids{{OrderId{2}, Side::Buy, Price{100}, Quantity{3}},
                                       {OrderId{1}, Side::Buy, Price{99}, Quantity{5}},
                                       {OrderId{3}, Side::Buy, Price{99}, Quantity{7}}};
  EXPECT_EQ(book.orders(Side::Buy), bids);
  const std::vector<RestingOrder> asks{{OrderId{5}, Side::Sell, Price{102}, Quantity{6}},
                                       {OrderId{4}, Side::Sell, Price{103}, Quantity{2}},
                                       {OrderId{6}, Side::Sell, Price{103}, Quantity{1}}};
  EXPECT_EQ(book.orders(Side::Sell), asks);
  EXPECT_EQ(format(book.depth(Side::Buy)), "100x3 99x12");
  EXPECT_EQ(format(book.depth(Side::Sell)), "102x6 103x3");
}

TEST(ReferenceBook, ChecksTheRulesInTheirSpecifiedOrder) {
  ReferenceBook book;
  run(book, "new 5 buy limit 1 @ 100");
  // Breaks all three rules: the id is reported, because it is checked first.
  EXPECT_EQ(run(book, "new 5 buy limit 0 @ 0"), Lines{"rejected 5 id-not-increasing"});
  // Breaks the last two: the quantity is reported.
  EXPECT_EQ(run(book, "new 6 buy limit 0 @ 0"), Lines{"rejected 6 invalid-quantity"});
  EXPECT_EQ(run(book, "new 6 buy limit 1 @ 0"), Lines{"rejected 6 invalid-price"});
}

TEST(ReferenceBook, IdsMustIncrease) {
  ReferenceBook book;
  EXPECT_EQ(run(book, "new 0 buy limit 1 @ 100"), Lines{"rejected 0 id-not-increasing"});
  EXPECT_EQ(run(book, "new 7 buy limit 1 @ 100"), Lines{"accepted 7"});
  EXPECT_EQ(run(book, "new 7 sell limit 1 @ 200"), Lines{"rejected 7 id-not-increasing"});
  EXPECT_EQ(run(book, "new 3 sell limit 1 @ 200"), Lines{"rejected 3 id-not-increasing"});
  // An id may skip ahead: it only has to be greater.
  EXPECT_EQ(run(book, "new 1000 sell limit 1 @ 200"), Lines{"accepted 1000"});
}

TEST(ReferenceBook, BoundsAreInclusive) {
  ReferenceBook book;
  EXPECT_EQ(run(book, "new 1 buy limit 1000000000 @ 1"), Lines{"accepted 1"});
  EXPECT_EQ(run(book, "new 2 sell limit 1 @ 1000000000"), Lines{"accepted 2"});
  EXPECT_EQ(run(book, "new 3 buy limit 1000000001 @ 1"), Lines{"rejected 3 invalid-quantity"});
  EXPECT_EQ(run(book, "new 3 sell limit 1 @ 1000000001"), Lines{"rejected 3 invalid-price"});
  EXPECT_EQ(run(book, "new 3 sell ioc 1 @ -5"), Lines{"rejected 3 invalid-price"});
}

TEST(ReferenceBook, AMarketOrderMustNotCarryAPrice) {
  ReferenceBook book;
  run(book, "new 1 sell limit 5 @ 100");
  EXPECT_EQ(run(book, "new 2 buy market 5 @ 100"), Lines{"rejected 2 invalid-price"});
  EXPECT_EQ(run(book, "new 2 buy market 5"), (Lines{"accepted 2", "trade 2 1 5 @ 100"}));
}

TEST(ReferenceBook, ARejectedCommandChangesNothing) {
  ReferenceBook book;
  run_all(book, {"new 1 buy limit 5 @ 99", "new 2 sell limit 6 @ 102", "new 3 sell limit 1 @ 103"});
  const auto bids = book.orders(Side::Buy);
  const auto asks = book.orders(Side::Sell);
  for (const char* line : {"new 3 buy market 100", "new 4 buy ioc 0 @ 103", "new 4 buy limit 6 @ 1000000001",
                           "new 4 sell market 9 @ 1", "cancel 99", "cancel 0"}) {
    const Lines events = run(book, line);
    ASSERT_EQ(events.size(), 1U) << line;
    EXPECT_EQ(events[0].rfind("rejected ", 0), 0U) << line;
    EXPECT_EQ(book.orders(Side::Buy), bids) << line;
    EXPECT_EQ(book.orders(Side::Sell), asks) << line;
    EXPECT_EQ(book.last_accepted_id(), OrderId{3}) << line;
  }
  // So the order can be sent again with the same id once it is fixed.
  EXPECT_EQ(run(book, "new 4 buy ioc 1 @ 103"), (Lines{"accepted 4", "trade 4 2 1 @ 102"}));
}

TEST(ReferenceBook, CancelReportsWhatWasLeft) {
  ReferenceBook book;
  run_all(book, {"new 1 sell limit 10 @ 101", "new 2 buy ioc 4 @ 101"});
  EXPECT_EQ(run(book, "cancel 1"), Lines{"cancelled 1 6 requested"});
  EXPECT_EQ(book.size(), 0U);
  EXPECT_EQ(run(book, "cancel 1"), Lines{"rejected 1 unknown-order"});
}

TEST(ReferenceBook, OnlyRestingOrdersCanBeCancelled) {
  ReferenceBook book;
  run_all(book, {"new 1 sell limit 3 @ 101", "new 2 buy limit 3 @ 101", "new 3 buy ioc 3 @ 100",
                 "new 4 buy market 1", "new 5 buy limit 0 @ 100"});
  EXPECT_EQ(run(book, "cancel 1"), Lines{"rejected 1 unknown-order"});  // filled
  EXPECT_EQ(run(book, "cancel 2"), Lines{"rejected 2 unknown-order"});  // filled as it arrived
  EXPECT_EQ(run(book, "cancel 3"), Lines{"rejected 3 unknown-order"});  // an IOC never rests
  EXPECT_EQ(run(book, "cancel 4"), Lines{"rejected 4 unknown-order"});  // nor does a market order
  EXPECT_EQ(run(book, "cancel 5"), Lines{"rejected 5 unknown-order"});  // rejected
  EXPECT_EQ(run(book, "cancel 6"), Lines{"rejected 6 unknown-order"});  // never sent
}

TEST(ReferenceBook, ATakerSweepsLevelsBestFirstAtEachMakersPrice) {
  ReferenceBook book;
  run_all(book, {"new 1 buy limit 5 @ 98", "new 2 buy limit 5 @ 100", "new 3 buy limit 5 @ 99"});
  EXPECT_EQ(run(book, "new 4 sell limit 12 @ 98"),
            (Lines{"accepted 4", "trade 4 2 5 @ 100", "trade 4 3 5 @ 99", "trade 4 1 2 @ 98"}));
  EXPECT_EQ(format(book.depth(Side::Buy)), "98x3");
  EXPECT_TRUE(book.depth(Side::Sell).empty());
}

}  // namespace
}  // namespace orderbook
