// The depth feed and its subscriber: each command's updates name the levels it changed, once each, with their new
// totals; after every command of long random streams the feed's levels are the book's depth, and so are those of a
// subscriber that applied every update; a subscriber that joins late from a snapshot ends up the same; and a gap in
// the updates is refused.

#include "orderbook/market_data.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "orderbook/book.hpp"
#include "orderbook/random_flow.hpp"
#include "orderbook/reference_book.hpp"
#include "orderbook/text.hpp"

namespace orderbook {
namespace {

using Lines = std::vector<std::string>;

// A book and a feed following it, one command line at a time.
struct Followed {
  Book book;
  DepthFeed feed;

  // The command's level updates, in their text form.
  Lines run(std::string_view line) {
    std::string error;
    const auto command = parse_command(line, &error);
    EXPECT_TRUE(command.has_value()) << line << ": " << error;
    if (!command) return {};
    std::vector<Event> events;
    book.apply(*command, events);
    std::vector<LevelUpdate> updates;
    feed.apply(*command, events, updates);
    Lines out;
    for (const LevelUpdate& update : updates) out.push_back(format(update));
    return out;
  }
};

TEST(DepthFeed, ARestingOrderMakesOrRaisesItsLevel) {
  Followed f;
  EXPECT_EQ(f.run("new 1 buy limit 10 @ 100"), (Lines{"level buy 100x10"}));
  EXPECT_EQ(f.run("new 2 buy limit 5 @ 100"), (Lines{"level buy 100x15"}));
  EXPECT_EQ(f.run("new 3 sell limit 7 @ 102"), (Lines{"level sell 102x7"}));
  EXPECT_EQ(format(f.feed.snapshot(kAllLevels)), "snapshot 3 bids 100x15 asks 102x7");
  EXPECT_EQ(f.feed.resting(), 3U);
}

TEST(DepthFeed, ASweepNamesEachLevelOnceThenTheRemaindersLevel) {
  Followed f;
  f.run("new 1 sell limit 5 @ 101");
  f.run("new 2 sell limit 3 @ 101");
  f.run("new 3 sell limit 4 @ 102");
  f.run("new 4 sell limit 1 @ 102");
  f.run("new 5 sell limit 9 @ 104");
  // Four makers at two levels: two updates, not four, then the 7 lots left resting at 103.
  EXPECT_EQ(f.run("new 6 buy limit 20 @ 103"), (Lines{"level sell 101x0", "level sell 102x0", "level buy 103x7"}));
  EXPECT_EQ(f.feed.depth(Side::Sell), f.book.depth(Side::Sell));
  EXPECT_EQ(f.feed.depth(Side::Buy), f.book.depth(Side::Buy));
  EXPECT_EQ(format(f.feed.snapshot(kAllLevels)), "snapshot 6 bids 103x7 asks 104x9");
}

TEST(DepthFeed, APartialFillLeavesTheLevelWithWhatIsLeft) {
  Followed f;
  f.run("new 1 sell limit 5 @ 101");
  f.run("new 2 sell limit 3 @ 101");
  EXPECT_EQ(f.run("new 3 buy ioc 6 @ 101"), (Lines{"level sell 101x2"}));
  EXPECT_EQ(f.feed.resting(), 1U);
}

TEST(DepthFeed, IocAndMarketRemaindersNeverRest) {
  Followed f;
  f.run("new 1 sell limit 5 @ 101");
  EXPECT_EQ(f.run("new 2 buy ioc 8 @ 101"), (Lines{"level sell 101x0"}));  // 3 lots left, cancelled
  EXPECT_EQ(f.run("new 3 buy market 4"), Lines{});                          // nothing to trade with
  EXPECT_EQ(f.run("new 4 sell ioc 4 @ 90"), Lines{});
  EXPECT_EQ(f.feed.resting(), 0U);
  EXPECT_EQ(format(f.feed.snapshot(kAllLevels)), "snapshot 4 bids - asks -");
}

TEST(DepthFeed, ACancelTakesWhatWasLeftFromItsLevel) {
  Followed f;
  f.run("new 1 buy limit 10 @ 100");
  f.run("new 2 buy limit 4 @ 100");
  f.run("new 3 sell ioc 6 @ 100");  // order 1 has 4 left
  EXPECT_EQ(f.run("cancel 1"), (Lines{"level buy 100x4"}));
  EXPECT_EQ(f.run("cancel 2"), (Lines{"level buy 100x0"}));
  EXPECT_EQ(f.run("cancel 2"), Lines{});  // rejected: no longer resting
}

TEST(DepthFeed, RejectionsChangeNoLevelButTakeASequenceNumber) {
  Followed f;
  f.run("new 5 buy limit 10 @ 100");
  EXPECT_EQ(f.run("new 5 buy limit 10 @ 100"), Lines{});  // id-not-increasing
  EXPECT_EQ(f.run("new 6 buy limit 0 @ 100"), Lines{});   // invalid-quantity
  EXPECT_EQ(f.run("cancel 99"), Lines{});                 // unknown-order
  EXPECT_EQ(f.feed.sequence(), 4U);
  EXPECT_EQ(format(f.feed.snapshot(kAllLevels)), "snapshot 4 bids 100x10 asks -");
}

TEST(DepthFeed, ACommandsUpdatesAreOneLineAndOneWithNoneIsJustItsNumber) {
  const std::vector<LevelUpdate> updates{{Side::Sell, Price{101}, Quantity{0}}, {Side::Buy, Price{101}, Quantity{3}}};
  EXPECT_EQ(format_updates(8, updates), "update 8 sell 101x0 buy 101x3");
  EXPECT_EQ(format_updates(9, std::vector<LevelUpdate>{}), "update 9");
}

TEST(DepthFeed, ASnapshotHasUpToTheLevelsAskedForBestFirst) {
  Followed f;
  for (const std::string_view line : {"new 1 buy limit 1 @ 98", "new 2 buy limit 2 @ 100", "new 3 buy limit 3 @ 99",
                                      "new 4 sell limit 4 @ 103", "new 5 sell limit 5 @ 101", "new 6 sell limit 6 @ 102"}) {
    f.run(line);
  }
  EXPECT_EQ(format(f.feed.snapshot(2)), "snapshot 6 bids 100x2 99x3 asks 101x5 102x6");
  EXPECT_EQ(format(f.feed.snapshot(kAllLevels)), "snapshot 6 bids 100x2 99x3 98x1 asks 101x5 102x6 103x4");
  EXPECT_EQ(format(f.feed.snapshot(0)), "snapshot 6 bids - asks -");
  EXPECT_EQ(format(DepthFeed{}.snapshot(5)), "snapshot 0 bids - asks -");
}

TEST(DepthFeed, EventsThatDontFitItsRecordThrow) {
  std::vector<LevelUpdate> updates;
  const NewOrder sell{OrderId{1}, Side::Sell, OrderType::Limit, Quantity{5}, Price{101}};
  // A trade with an order the feed never saw rest.
  {
    DepthFeed feed;
    const std::vector<Event> events{Accepted{OrderId{2}}, Trade{OrderId{2}, OrderId{1}, Quantity{5}, Price{101}}};
    EXPECT_THROW(feed.apply(NewOrder{OrderId{2}, Side::Buy, OrderType::Limit, Quantity{5}, Price{101}}, events,
                            updates),
                 std::logic_error);
  }
  DepthFeed feed;
  feed.apply(sell, std::vector<Event>{Accepted{OrderId{1}}}, updates);
  // A trade at a price other than the maker's.
  EXPECT_THROW(feed.apply(NewOrder{OrderId{2}, Side::Buy, OrderType::Limit, Quantity{5}, Price{102}},
                          std::vector<Event>{Accepted{OrderId{2}}, Trade{OrderId{2}, OrderId{1}, Quantity{5}, Price{102}}},
                          updates),
               std::logic_error);
  // For more than the taker has.
  EXPECT_THROW(feed.apply(NewOrder{OrderId{3}, Side::Buy, OrderType::Limit, Quantity{4}, Price{101}},
                          std::vector<Event>{Accepted{OrderId{3}}, Trade{OrderId{3}, OrderId{1}, Quantity{5}, Price{101}}},
                          updates),
               std::logic_error);
  // A cancellation of more than is left of the order.
  EXPECT_THROW(feed.apply(CancelOrder{OrderId{1}},
                          std::vector<Event>{Cancelled{OrderId{1}, Quantity{6}, CancelReason::Requested}}, updates),
               std::logic_error);
  // A trade naming another order as the taker.
  EXPECT_THROW(feed.apply(NewOrder{OrderId{5}, Side::Buy, OrderType::Limit, Quantity{5}, Price{101}},
                          std::vector<Event>{Accepted{OrderId{5}}, Trade{OrderId{9}, OrderId{1}, Quantity{1}, Price{101}}},
                          updates),
               std::logic_error);
  // A trade for more than the maker has left (the taker has room for it).
  EXPECT_THROW(feed.apply(NewOrder{OrderId{6}, Side::Buy, OrderType::Limit, Quantity{10}, Price{101}},
                          std::vector<Event>{Accepted{OrderId{6}}, Trade{OrderId{6}, OrderId{1}, Quantity{6}, Price{101}}},
                          updates),
               std::logic_error);
  // A trade with a maker on the taker's own side.
  EXPECT_THROW(feed.apply(NewOrder{OrderId{7}, Side::Sell, OrderType::Limit, Quantity{5}, Price{101}},
                          std::vector<Event>{Accepted{OrderId{7}}, Trade{OrderId{7}, OrderId{1}, Quantity{1}, Price{101}}},
                          updates),
               std::logic_error);
  // A cancellation of less than is left, and one of another order than the cancel names.
  EXPECT_THROW(feed.apply(CancelOrder{OrderId{1}},
                          std::vector<Event>{Cancelled{OrderId{1}, Quantity{4}, CancelReason::Requested}}, updates),
               std::logic_error);
  EXPECT_THROW(feed.apply(CancelOrder{OrderId{1}},
                          std::vector<Event>{Cancelled{OrderId{2}, Quantity{5}, CancelReason::Requested}}, updates),
               std::logic_error);
  // Each check comes before any change, so order 1 is still there, whole, and can be cancelled. (The header promises
  // less: after a throw, the feed isn't to be used.)
  updates.clear();
  feed.apply(CancelOrder{OrderId{1}}, std::vector<Event>{Cancelled{OrderId{1}, Quantity{5}, CancelReason::Requested}},
             updates);
  EXPECT_EQ(updates, (std::vector<LevelUpdate>{{Side::Sell, Price{101}, Quantity{0}}}));
  // No events at all for a new order.
  EXPECT_THROW(feed.apply(NewOrder{OrderId{4}, Side::Buy, OrderType::Limit, Quantity{1}, Price{1}},
                          std::vector<Event>{}, updates),
               std::logic_error);
}

// After every `every`th command of a random stream, the feed's levels and a subscriber's are the book's depth, and
// the feed's record has the book's number of resting orders. Every command's updates name each level at most once.
// The book must reach `min_largest` resting orders at some point (each floor is below what its stream reached).
template <typename AnyBook>
void expect_the_feed_follows(std::uint64_t seed, const FlowOptions& options, std::size_t commands, std::size_t every,
                             std::size_t min_largest) {
  AnyBook book;
  DepthFeed feed;
  L2Book subscriber;
  RandomFlow flow(seed, options);
  std::vector<Event> events;
  std::vector<LevelUpdate> updates;
  std::size_t changed = 0;
  std::size_t largest = 0;
  for (std::size_t i = 0; i < commands; ++i) {
    const Command command = flow.next();
    events.clear();
    updates.clear();
    book.apply(command, events);
    const std::uint64_t sequence = feed.apply(command, events, updates);
    ASSERT_EQ(sequence, i + 1);
    ASSERT_TRUE(subscriber.apply(sequence, updates));
    for (std::size_t j = 0; j < updates.size(); ++j) {
      for (std::size_t k = 0; k < j; ++k) {
        ASSERT_FALSE(updates[j].side == updates[k].side && updates[j].price == updates[k].price)
            << format(updates[j]) << " twice for " << format(command);
      }
    }
    changed += updates.size();
    largest = std::max(largest, book.size());
    if ((i + 1) % every != 0 && i + 1 != commands) continue;
    for (const Side side : {Side::Buy, Side::Sell}) {
      const std::vector<Level> depth = book.depth(side);
      ASSERT_EQ(format(feed.depth(side)), format(depth)) << "after command " << i << ", " << format(command);
      ASSERT_EQ(format(subscriber.depth(side)), format(depth)) << "after command " << i << ", " << format(command);
    }
    ASSERT_EQ(feed.resting(), book.size()) << "after command " << i;
  }
  EXPECT_GT(changed, commands / 2);  // the stream did move levels
  EXPECT_GE(largest, min_largest);   // and built the book it was meant to
}

TEST(DepthFeed, FollowsTheEngineAfterEveryCommandOfRandomStreams) {
  // The default mix; three ticks wide (long queues); 30% spoiled orders; quantities to 10^9 on a near-empty book.
  // (They reached 337, 98, 84 and 23 resting orders when written.)
  expect_the_feed_follows<Book>(1, FlowOptions{}, 20'000, 1, 300);
  expect_the_feed_follows<Book>(11, FlowOptions{.price_range = 3, .max_quantity = 20, .target_resting = 200}, 20'000,
                                1, 80);
  expect_the_feed_follows<Book>(14, FlowOptions{.target_resting = 100, .invalid_per_mille = 300}, 20'000, 1, 70);
  expect_the_feed_follows<Book>(16, FlowOptions{.max_quantity = 1'000'000'000, .target_resting = 5}, 20'000, 1, 15);
}

TEST(DepthFeed, FollowsTheEngineOnADeepBook) {
  // Up to 1,270 resting orders over 4,000 ticks: compared every 50 commands, since each comparison walks every level.
  expect_the_feed_follows<Book>(15, FlowOptions{.price_range = 2'000, .target_resting = 2'000}, 20'000, 50, 1'000);
}

TEST(DepthFeed, FollowsTheReferenceModel) {
  expect_the_feed_follows<ReferenceBook>(2, FlowOptions{.target_resting = 200}, 10'000, 1, 120);  // reached 146
}

TEST(L2Book, ALateJoinerFromAFullSnapshotEndsUpLikeOneThatFollowedFromTheStart) {
  Book book;
  DepthFeed feed;
  L2Book early;
  std::optional<L2Book> late;
  RandomFlow flow(3, FlowOptions{.target_resting = 300});
  std::vector<Event> events;
  std::vector<LevelUpdate> updates;
  for (std::size_t i = 0; i < 10'000; ++i) {
    const Command command = flow.next();
    events.clear();
    updates.clear();
    book.apply(command, events);
    const std::uint64_t sequence = feed.apply(command, events, updates);
    ASSERT_TRUE(early.apply(sequence, updates));
    if (late) {
      ASSERT_TRUE(late->apply(sequence, updates));
    } else if (i == 4'999) {
      late.emplace(feed.snapshot(kAllLevels));  // joins after command 5,000
      EXPECT_EQ(late->sequence(), 5'000U);
    }
  }
  ASSERT_TRUE(late.has_value());
  EXPECT_EQ(late->snapshot(kAllLevels), early.snapshot(kAllLevels));
  EXPECT_EQ(late->snapshot(kAllLevels), feed.snapshot(kAllLevels));
  EXPECT_EQ(late->sequence(), 10'000U);
  EXPECT_GT(late->depth(Side::Buy).size() + late->depth(Side::Sell).size(), 20U);
}

TEST(L2Book, UpdatesOutOfSequenceAreRefusedAndChangeNothing) {
  L2Book book;
  const std::vector<LevelUpdate> updates{{Side::Buy, Price{100}, Quantity{5}}};
  EXPECT_FALSE(book.apply(2, updates));  // a gap: command 1's updates are missing
  EXPECT_EQ(book.sequence(), 0U);
  EXPECT_TRUE(book.depth(Side::Buy).empty());
  EXPECT_TRUE(book.apply(1, updates));
  EXPECT_FALSE(book.apply(1, updates));  // a repeat
  EXPECT_TRUE(book.apply(2, std::vector<LevelUpdate>{}));  // a command that changed no level
  EXPECT_EQ(format(book.snapshot(kAllLevels)), "snapshot 2 bids 100x5 asks -");
}

TEST(L2Book, ATotalOfZeroRemovesTheLevel) {
  L2Book book(L2Snapshot{7, {{Price{100}, Quantity{5}}, {Price{99}, Quantity{2}}}, {{Price{101}, Quantity{3}}}});
  EXPECT_EQ(format(book.snapshot(kAllLevels)), "snapshot 7 bids 100x5 99x2 asks 101x3");
  EXPECT_TRUE(book.apply(8, std::vector<LevelUpdate>{{Side::Buy, Price{100}, Quantity{0}},
                                                     {Side::Sell, Price{102}, Quantity{4}}}));
  EXPECT_EQ(format(book.snapshot(kAllLevels)), "snapshot 8 bids 99x2 asks 101x3 102x4");
}

}  // namespace
}  // namespace orderbook
