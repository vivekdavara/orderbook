// The rules, one at a time, on every book: the reference model, the engine, and day 2's engine (the baseline) must
// each pass every test here.

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "orderbook/book.hpp"
#include "orderbook/map_book.hpp"
#include "orderbook/reference_book.hpp"
#include "orderbook/text.hpp"

namespace orderbook {
namespace {

using Lines = std::vector<std::string>;

// Applies one command written in the text format and returns its events in the text format.
template <typename AnyBook>
Lines run(AnyBook& book, std::string_view line) {
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

template <typename AnyBook>
void run_all(AnyBook& book, const std::vector<std::string_view>& lines) {
  for (const std::string_view line : lines) run(book, line);
}

// CTest names each test after its type: Books.StartsEmpty<orderbook::ReferenceBook>, then <orderbook::Book>, then
// <orderbook::MapBook>.
template <typename AnyBook>
class Books : public ::testing::Test {};

using Implementations = ::testing::Types<ReferenceBook, Book, MapBook>;
TYPED_TEST_SUITE(Books, Implementations);

TYPED_TEST(Books, StartsEmpty) {
  const TypeParam book;
  EXPECT_EQ(book.size(), 0U);
  EXPECT_EQ(book.best_price(Side::Buy), std::nullopt);
  EXPECT_EQ(book.best_price(Side::Sell), std::nullopt);
  EXPECT_TRUE(book.depth(Side::Buy).empty());
  EXPECT_EQ(book.last_accepted_id(), OrderId{0});
}

TYPED_TEST(Books, RestsALimitOrderThatDoesNotCross) {
  TypeParam book;
  EXPECT_EQ(run(book, "new 1 buy limit 10 @ 100"), Lines{"accepted 1"});
  EXPECT_EQ(run(book, "new 2 sell limit 4 @ 101"), Lines{"accepted 2"});
  EXPECT_EQ(book.size(), 2U);
  EXPECT_EQ(book.best_price(Side::Buy), Price{100});
  EXPECT_EQ(book.best_price(Side::Sell), Price{101});
  EXPECT_EQ(book.orders(Side::Buy), (std::vector<RestingOrder>{{OrderId{1}, Side::Buy, Price{100}, Quantity{10}}}));
  EXPECT_EQ(book.last_accepted_id(), OrderId{2});
}

TYPED_TEST(Books, ReportsOrdersInPriorityOrderAndDepthBestFirst) {
  TypeParam book;
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
  // The best of several levels on each side, and six orders (not four levels) resting.
  EXPECT_EQ(book.best_price(Side::Buy), Price{100});
  EXPECT_EQ(book.best_price(Side::Sell), Price{102});
  EXPECT_EQ(book.size(), 6U);
}

TYPED_TEST(Books, ChecksTheRulesInTheirSpecifiedOrder) {
  TypeParam book;
  run(book, "new 5 buy limit 1 @ 100");
  // Breaks all three rules: the id is reported, because it is checked first.
  EXPECT_EQ(run(book, "new 5 buy limit 0 @ 0"), Lines{"rejected 5 id-not-increasing"});
  // Breaks the last two: the quantity is reported.
  EXPECT_EQ(run(book, "new 6 buy limit 0 @ 0"), Lines{"rejected 6 invalid-quantity"});
  EXPECT_EQ(run(book, "new 6 buy limit 1 @ 0"), Lines{"rejected 6 invalid-price"});
}

TYPED_TEST(Books, IdsMustIncrease) {
  TypeParam book;
  EXPECT_EQ(run(book, "new 0 buy limit 1 @ 100"), Lines{"rejected 0 id-not-increasing"});
  EXPECT_EQ(run(book, "new 7 buy limit 1 @ 100"), Lines{"accepted 7"});
  EXPECT_EQ(run(book, "new 7 sell limit 1 @ 200"), Lines{"rejected 7 id-not-increasing"});
  EXPECT_EQ(run(book, "new 3 sell limit 1 @ 200"), Lines{"rejected 3 id-not-increasing"});
  // An id may skip ahead: it only has to be greater, by any amount, up to the largest id.
  EXPECT_EQ(run(book, "new 1000 sell limit 1 @ 200"), Lines{"accepted 1000"});
  EXPECT_EQ(run(book, "new 5000000000 sell limit 1 @ 201"), Lines{"accepted 5000000000"});
  EXPECT_EQ(run(book, "new 18446744073709551615 sell limit 1 @ 202"), Lines{"accepted 18446744073709551615"});
  EXPECT_EQ(book.last_accepted_id(), OrderId{18446744073709551615ULL});
  // After the largest id nothing more can be accepted.
  EXPECT_EQ(run(book, "new 18446744073709551615 buy market 1"),
            Lines{"rejected 18446744073709551615 id-not-increasing"});
}

TYPED_TEST(Books, BoundsAreInclusive) {
  TypeParam book;
  EXPECT_EQ(run(book, "new 1 buy limit 1000000000 @ 1"), Lines{"accepted 1"});
  EXPECT_EQ(run(book, "new 2 sell limit 1 @ 1000000000"), Lines{"accepted 2"});
  EXPECT_EQ(run(book, "new 3 buy limit 1000000001 @ 1"), Lines{"rejected 3 invalid-quantity"});
  EXPECT_EQ(run(book, "new 3 sell limit 1 @ 1000000001"), Lines{"rejected 3 invalid-price"});
  EXPECT_EQ(run(book, "new 3 sell ioc 1 @ -5"), Lines{"rejected 3 invalid-price"});
}

TYPED_TEST(Books, AMarketOrderTradesWithMakersAtThePriceBounds) {
  TypeParam book;
  run_all(book, {"new 1 sell limit 3 @ 1000000000", "new 2 buy limit 4 @ 1"});
  EXPECT_EQ(run(book, "new 3 buy market 5"),
            (Lines{"accepted 3", "trade 3 1 3 @ 1000000000", "cancelled 3 2 unfilled"}));
  EXPECT_EQ(run(book, "new 4 sell market 4"), (Lines{"accepted 4", "trade 4 2 4 @ 1"}));
  EXPECT_EQ(book.size(), 0U);
}

TYPED_TEST(Books, AMarketOrderMustNotCarryAPrice) {
  TypeParam book;
  run(book, "new 1 sell limit 5 @ 100");
  EXPECT_EQ(run(book, "new 2 buy market 5 @ 100"), Lines{"rejected 2 invalid-price"});
  EXPECT_EQ(run(book, "new 2 buy market 5"), (Lines{"accepted 2", "trade 2 1 5 @ 100"}));
}

TYPED_TEST(Books, ARejectedCommandChangesNothing) {
  TypeParam book;
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
    EXPECT_EQ(book.size(), 3U) << line;
  }
  // So the order can be sent again with the same id once it is fixed.
  EXPECT_EQ(run(book, "new 4 buy ioc 1 @ 103"), (Lines{"accepted 4", "trade 4 2 1 @ 102"}));
}

TYPED_TEST(Books, CancelReportsWhatWasLeft) {
  TypeParam book;
  run_all(book, {"new 1 sell limit 10 @ 101", "new 2 buy ioc 4 @ 101"});
  EXPECT_EQ(run(book, "cancel 1"), Lines{"cancelled 1 6 requested"});
  EXPECT_EQ(book.size(), 0U);
  EXPECT_EQ(run(book, "cancel 1"), Lines{"rejected 1 unknown-order"});
}

TYPED_TEST(Books, OnlyRestingOrdersCanBeCancelled) {
  TypeParam book;
  run_all(book, {"new 1 sell limit 3 @ 101", "new 2 buy limit 3 @ 101", "new 3 buy ioc 3 @ 100",
                 "new 4 buy market 1", "new 5 buy limit 0 @ 100"});
  EXPECT_EQ(run(book, "cancel 1"), Lines{"rejected 1 unknown-order"});  // filled
  EXPECT_EQ(run(book, "cancel 2"), Lines{"rejected 2 unknown-order"});  // filled as it arrived
  EXPECT_EQ(run(book, "cancel 3"), Lines{"rejected 3 unknown-order"});  // an IOC never rests
  EXPECT_EQ(run(book, "cancel 4"), Lines{"rejected 4 unknown-order"});  // nor does a market order
  EXPECT_EQ(run(book, "cancel 5"), Lines{"rejected 5 unknown-order"});  // rejected
  EXPECT_EQ(run(book, "cancel 6"), Lines{"rejected 6 unknown-order"});  // never sent
}

TYPED_TEST(Books, ATakerSweepsLevelsBestFirstAtEachMakersPrice) {
  TypeParam book;
  run_all(book, {"new 1 buy limit 5 @ 98", "new 2 buy limit 5 @ 100", "new 3 buy limit 5 @ 99"});
  EXPECT_EQ(run(book, "new 4 sell limit 12 @ 98"),
            (Lines{"accepted 4", "trade 4 2 5 @ 100", "trade 4 3 5 @ 99", "trade 4 1 2 @ 98"}));
  EXPECT_EQ(format(book.depth(Side::Buy)), "98x3");
  EXPECT_TRUE(book.depth(Side::Sell).empty());
}

}  // namespace
}  // namespace orderbook
