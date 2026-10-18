// The engine's own structure: its levels appear and go away with their orders, each level's total follows its
// queue, cancels unlink an order from anywhere in a queue, the pool reuses the nodes orders leave and grows past the
// room it was built with, and copies and moves keep every link. The rules
// themselves are tested on both books in books_test.cpp, and against the reference model in differential_test.cpp.

#include "orderbook/book.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "orderbook/text.hpp"

namespace orderbook {
namespace {

using Lines = std::vector<std::string>;

// Day 2's book was move-only, since its index held iterators into its own containers. Day 3's links nodes by index
// into its own pool, so a member-wise copy is a book of its own.
static_assert(std::is_copy_constructible_v<Book> && std::is_copy_assignable_v<Book>);
static_assert(std::is_nothrow_move_constructible_v<Book> && std::is_nothrow_move_assignable_v<Book>);

Lines run(Book& book, std::string_view line) {
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

void run_all(Book& book, const std::vector<std::string_view>& lines) {
  for (const std::string_view line : lines) run(book, line);
}

TEST(Engine, ALevelFilledAwayLeavesTheBook) {
  Book book;
  run_all(book, {"new 1 sell limit 5 @ 101", "new 2 sell limit 3 @ 101", "new 3 sell limit 4 @ 102"});
  EXPECT_EQ(format(book.depth(Side::Sell)), "101x8 102x4");
  EXPECT_EQ(run(book, "new 4 buy ioc 8 @ 101"), (Lines{"accepted 4", "trade 4 1 5 @ 101", "trade 4 2 3 @ 101"}));
  EXPECT_EQ(format(book.depth(Side::Sell)), "102x4");
  EXPECT_EQ(book.best_price(Side::Sell), Price{102});
  EXPECT_EQ(book.size(), 1U);
}

TEST(Engine, ALevelCancelledAwayLeavesTheBook) {
  Book book;
  run_all(book, {"new 1 buy limit 5 @ 100", "new 2 buy limit 3 @ 100", "new 3 buy limit 4 @ 99"});
  run_all(book, {"cancel 2", "cancel 1"});
  EXPECT_EQ(format(book.depth(Side::Buy)), "99x4");
  EXPECT_EQ(book.best_price(Side::Buy), Price{99});
  run(book, "cancel 3");
  EXPECT_TRUE(book.depth(Side::Buy).empty());
  EXPECT_EQ(book.best_price(Side::Buy), std::nullopt);
  EXPECT_EQ(book.size(), 0U);
}

TEST(Engine, ALevelsTotalFollowsFillsAndCancels) {
  Book book;
  run_all(book, {"new 1 sell limit 10 @ 101", "new 2 sell limit 7 @ 101", "new 3 sell limit 2 @ 101"});
  EXPECT_EQ(format(book.depth(Side::Sell)), "101x19");
  run(book, "new 4 buy limit 4 @ 101");  // a partial fill of order 1
  EXPECT_EQ(format(book.depth(Side::Sell)), "101x15");
  run(book, "cancel 2");
  EXPECT_EQ(format(book.depth(Side::Sell)), "101x8");
  EXPECT_EQ(book.orders(Side::Sell), (std::vector<RestingOrder>{{OrderId{1}, Side::Sell, Price{101}, Quantity{6}},
                                                                {OrderId{3}, Side::Sell, Price{101}, Quantity{2}}}));
}

TEST(Engine, ACancelFromAnywhereInAQueueKeepsTheRestInOrder) {
  for (const char* cancelled : {"cancel 1", "cancel 2", "cancel 3"}) {
    Book book;
    run_all(book, {"new 1 buy limit 1 @ 100", "new 2 buy limit 1 @ 100", "new 3 buy limit 1 @ 100"});
    run(book, cancelled);
    const Lines events = run(book, "new 4 sell market 2");
    Lines expected{"accepted 4"};
    for (const char* maker : {"1", "2", "3"}) {
      if (std::string_view(cancelled).substr(7) != maker) {
        expected.push_back(std::string("trade 4 ") + maker + " 1 @ 100");
      }
    }
    EXPECT_EQ(events, expected) << cancelled;
    EXPECT_EQ(book.size(), 0U) << cancelled;
  }
}

TEST(Engine, APartlyFilledMakerStaysAtTheFront) {
  Book book;
  run_all(book, {"new 1 sell limit 10 @ 101", "new 2 sell limit 10 @ 101"});
  run(book, "new 3 buy ioc 4 @ 101");
  EXPECT_EQ(run(book, "new 4 buy ioc 8 @ 101"), (Lines{"accepted 4", "trade 4 1 6 @ 101", "trade 4 2 2 @ 101"}));
}

TEST(Engine, AnOrderAtAPriceWhoseLevelEmptiedStartsANewQueue) {
  Book book;
  run_all(book, {"new 1 sell limit 5 @ 101", "new 2 buy limit 5 @ 101"});  // fills 1; the level goes
  EXPECT_EQ(book.size(), 0U);
  EXPECT_TRUE(book.depth(Side::Sell).empty());
  EXPECT_EQ(book.best_price(Side::Sell), std::nullopt);
  run_all(book, {"new 3 sell limit 2 @ 101", "new 4 sell limit 2 @ 101"});
  EXPECT_EQ(format(book.depth(Side::Sell)), "101x4");
  EXPECT_EQ(run(book, "new 5 buy market 3"), (Lines{"accepted 5", "trade 5 3 2 @ 101", "trade 5 4 1 @ 101"}));
  EXPECT_EQ(run(book, "cancel 4"), Lines{"cancelled 4 1 requested"});
  EXPECT_TRUE(book.depth(Side::Sell).empty());
}

TEST(Engine, AMarketOrderSweepsEveryLevel) {
  Book book;
  for (int i = 0; i < 1'000; ++i) {
    run(book, "new " + std::to_string(i + 1) + " sell limit 1 @ " + std::to_string(1'000 + i));
  }
  EXPECT_EQ(book.depth(Side::Sell).size(), 1'000U);
  const Lines events = run(book, "new 1001 buy market 1000");
  ASSERT_EQ(events.size(), 1'001U);
  EXPECT_EQ(events[1], "trade 1001 1 1 @ 1000");
  EXPECT_EQ(events.back(), "trade 1001 1000 1 @ 1999");
  EXPECT_EQ(book.size(), 0U);
  EXPECT_TRUE(book.depth(Side::Sell).empty());
}

TEST(Engine, AMovedBookKeepsAWorkingIndex) {
  auto original = std::make_unique<Book>();
  run_all(*original, {"new 1 buy limit 5 @ 100", "new 2 buy limit 5 @ 100", "new 3 sell limit 5 @ 102"});
  Book moved(std::move(*original));
  // A move that copied the containers would leave the index pointing into the moved-from book, and this cancel would
  // unlink a node there: the event would look right, but this book's own orders and depth wouldn't change.
  EXPECT_EQ(run(moved, "cancel 1"), Lines{"cancelled 1 5 requested"});
  EXPECT_EQ(moved.orders(Side::Buy), (std::vector<RestingOrder>{{OrderId{2}, Side::Buy, Price{100}, Quantity{5}}}));
  EXPECT_EQ(format(moved.depth(Side::Buy)), "100x5");
  // Nothing may depend on the moved-from book once it is gone (under ASan, a use of it would be reported).
  original.reset();
  EXPECT_EQ(run(moved, "new 4 sell market 2"), (Lines{"accepted 4", "trade 4 2 2 @ 100"}));
  EXPECT_EQ(format(moved.depth(Side::Buy)), "100x3");
  EXPECT_EQ(moved.last_accepted_id(), OrderId{4});

  Book assigned;
  run(assigned, "new 1 sell limit 1 @ 500");  // replaced by the assignment
  auto source = std::make_unique<Book>(std::move(moved));
  assigned = std::move(*source);
  EXPECT_EQ(format(assigned.depth(Side::Sell)), "102x5");  // order 1 at 500 went with the old contents
  EXPECT_EQ(run(assigned, "cancel 2"), Lines{"cancelled 2 3 requested"});
  EXPECT_TRUE(assigned.orders(Side::Buy).empty());
  EXPECT_TRUE(assigned.depth(Side::Buy).empty());
  source.reset();
  EXPECT_EQ(run(assigned, "cancel 3"), Lines{"cancelled 3 5 requested"});
  EXPECT_TRUE(assigned.depth(Side::Sell).empty());
  EXPECT_EQ(assigned.size(), 0U);
  EXPECT_EQ(run(assigned, "new 4 buy limit 1 @ 100"), Lines{"rejected 4 id-not-increasing"});
}

TEST(Engine, ThePoolReusesTheNodesOrdersLeave) {
  Book book;
  std::uint64_t id = 0;
  for (int round = 0; round < 10; ++round) {
    // 100 orders rest, then leave: the first half cancelled, the rest filled by one market order.
    for (int i = 0; i < 100; ++i) {
      run(book, "new " + std::to_string(++id) + " buy limit 1 @ " + std::to_string(100 + i % 7));
    }
    for (std::uint64_t cancelled = id - 99; cancelled <= id - 50; ++cancelled) {
      run(book, "cancel " + std::to_string(cancelled));
    }
    run(book, "new " + std::to_string(++id) + " sell market 50");
    ASSERT_EQ(book.size(), 0U) << round;
    // The pool made 100 nodes in the first round and has used the same ones since.
    ASSERT_EQ(book.pool_nodes(), 100U) << round;
  }
}

TEST(Engine, ABookGrowsPastTheRoomItWasBuiltWith) {
  // Room for 4 orders and 2 levels a side; 2,000 orders rest on 500 levels a side. The pool, the index and both
  // sides' level vectors all grow, and every order is still found, in order.
  Book book(BookCapacity{.orders = 4, .levels = 2});
  for (int i = 0; i < 1'000; ++i) {
    run(book, "new " + std::to_string(2 * i + 1) + " buy limit 1 @ " + std::to_string(1'000 + i % 500));
    run(book, "new " + std::to_string(2 * i + 2) + " sell limit 1 @ " + std::to_string(2'000 + i % 500));
  }
  EXPECT_EQ(book.size(), 2'000U);
  EXPECT_EQ(book.pool_nodes(), 2'000U);
  EXPECT_EQ(book.depth(Side::Buy).size(), 500U);
  EXPECT_EQ(book.depth(Side::Sell).size(), 500U);
  EXPECT_EQ(book.best_price(Side::Buy), Price{1'499});
  EXPECT_EQ(book.best_price(Side::Sell), Price{2'000});
  // Orders 1 and 1001 rest at 1,000 (the worst bid), 1001 behind 1.
  EXPECT_EQ(run(book, "cancel 1001"), Lines{"cancelled 1001 1 requested"});
  EXPECT_EQ(run(book, "new 2001 sell market 2000"), [] {
    Lines expected{"accepted 2001"};
    for (int price = 1'499; price >= 1'000; --price) {
      for (const int i : {price - 1'000, price - 1'000 + 500}) {
        if (2 * i + 1 == 1'001) continue;  // cancelled
        expected.push_back("trade 2001 " + std::to_string(2 * i + 1) + " 1 @ " + std::to_string(price));
      }
    }
    expected.push_back("cancelled 2001 1001 unfilled");
    return expected;
  }());
  EXPECT_TRUE(book.depth(Side::Buy).empty());
  EXPECT_EQ(book.size(), 1'000U);
}

TEST(Engine, ACopyIsABookOfItsOwn) {
  Book original;
  run_all(original, {"new 1 buy limit 5 @ 100", "new 2 buy limit 5 @ 100", "new 3 sell limit 5 @ 102"});
  Book copy = original;
  EXPECT_EQ(run(copy, "cancel 1"), Lines{"cancelled 1 5 requested"});
  EXPECT_EQ(run(copy, "new 4 sell market 7"), (Lines{"accepted 4", "trade 4 2 5 @ 100", "cancelled 4 2 unfilled"}));
  // The original still has both bids, in order, and its own last accepted id.
  EXPECT_EQ(original.orders(Side::Buy), (std::vector<RestingOrder>{{OrderId{1}, Side::Buy, Price{100}, Quantity{5}},
                                                                    {OrderId{2}, Side::Buy, Price{100}, Quantity{5}}}));
  EXPECT_EQ(original.last_accepted_id(), OrderId{3});
  EXPECT_EQ(run(original, "cancel 2"), Lines{"cancelled 2 5 requested"});
  EXPECT_EQ(copy.size(), 1U);
  // Assigning a copy replaces what was there.
  copy = original;
  EXPECT_EQ(copy.orders(Side::Buy), (std::vector<RestingOrder>{{OrderId{1}, Side::Buy, Price{100}, Quantity{5}}}));
  EXPECT_EQ(run(copy, "new 4 buy limit 1 @ 100"), Lines{"accepted 4"});
  EXPECT_EQ(format(copy.depth(Side::Buy)), "100x6");
  EXPECT_EQ(format(original.depth(Side::Buy)), "100x5");
}

TEST(Engine, AMovedFromBookIsEmptyAndCanBeUsedAgain) {
  Book book;
  run_all(book, {"new 1 buy limit 5 @ 100", "new 2 sell limit 5 @ 102", "cancel 1"});  // node 0 on the free list
  Book moved(std::move(book));
  EXPECT_EQ(book.size(), 0U);  // a moved-from book is empty
  EXPECT_EQ(book.pool_nodes(), 0U);
  EXPECT_EQ(book.last_accepted_id(), OrderId{0});
  EXPECT_TRUE(book.orders(Side::Sell).empty());
  // Used again, it doesn't reach for the free node it had: that node went with the move.
  EXPECT_EQ(run(book, "new 1 sell limit 3 @ 101"), Lines{"accepted 1"});
  EXPECT_EQ(run(book, "new 2 buy limit 1 @ 101"), (Lines{"accepted 2", "trade 2 1 1 @ 101"}));
  EXPECT_EQ(format(book.depth(Side::Sell)), "101x2");
  EXPECT_EQ(run(moved, "cancel 2"), Lines{"cancelled 2 5 requested"});

  Book assigned;
  assigned = std::move(moved);
  EXPECT_EQ(moved.size(), 0U);
  EXPECT_EQ(moved.pool_nodes(), 0U);
  EXPECT_EQ(run(moved, "new 5 buy limit 2 @ 99"), Lines{"accepted 5"});
  EXPECT_EQ(run(moved, "cancel 5"), Lines{"cancelled 5 2 requested"});
  EXPECT_EQ(assigned.last_accepted_id(), OrderId{2});
}

TEST(Engine, MovingABookIntoItselfChangesNothing) {
  Book book;
  run_all(book, {"new 1 buy limit 5 @ 100", "new 2 sell limit 5 @ 102", "cancel 1"});
  Book& same = book;  // through a reference, as generic code would do it
  book = std::move(same);
  EXPECT_EQ(book.size(), 1U);
  EXPECT_EQ(book.last_accepted_id(), OrderId{2});
  EXPECT_EQ(format(book.depth(Side::Sell)), "102x5");
  EXPECT_EQ(run(book, "new 3 buy limit 1 @ 99"), Lines{"accepted 3"});  // takes the free node
  EXPECT_EQ(book.pool_nodes(), 2U);
}

}  // namespace
}  // namespace orderbook
