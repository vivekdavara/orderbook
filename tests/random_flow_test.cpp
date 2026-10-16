#include "orderbook/random_flow.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "orderbook/reference_book.hpp"
#include "orderbook/text.hpp"

namespace orderbook {
namespace {

// FNV-1a over each command's text line.
std::uint64_t hash_stream(RandomFlow& flow, int count) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (int i = 0; i < count; ++i) {
    for (const char c : format(flow.next()) + "\n") {
      hash ^= static_cast<unsigned char>(c);
      hash *= 0x100000001b3ULL;
    }
  }
  return hash;
}

TEST(RandomFlow, TheSameSeedGivesTheSameStream) {
  RandomFlow a(7);
  RandomFlow b(7);
  for (int i = 0; i < 10'000; ++i) ASSERT_EQ(a.next(), b.next()) << "command " << i;
}

TEST(RandomFlow, DifferentSeedsGiveDifferentStreams) {
  RandomFlow a(7);
  RandomFlow b(8);
  EXPECT_NE(hash_stream(a, 100), hash_stream(b, 100));
}

TEST(RandomFlow, TheStreamIsTheSameOnEveryPlatform) {
  // Recorded from this implementation, not derived: CI runs this on Linux with libstdc++ (GCC and Clang) and on
  // macOS with libc++, in Debug and Release, so it fails if the stream depends on anything but the seed. If the
  // generator is changed on purpose, this number changes with it, and the seeds in failing-test reports that
  // predate the change no longer reproduce.
  RandomFlow flow(1);
  EXPECT_EQ(hash_stream(flow, 100'000), 0x167c7c6d660e50beULL);
}

TEST(RandomFlow, EveryCommandHasATextFormThatParsesBack) {
  RandomFlow flow(3);
  for (int i = 0; i < 20'000; ++i) {
    const Command command = flow.next();
    ASSERT_EQ(parse_command(format(command)), command) << format(command);
  }
}

TEST(RandomFlow, MixesCommandsAsDocumented) {
  // Below the target number of cancel candidates: 60 limit, 10 IOC, 5 market and 25 cancels in 100; at or above it,
  // 35 limit and 50 cancels. Which applies is known before each command from cancel_candidates().
  RandomFlow flow(11);
  struct Mix {
    int commands = 0, limit = 0, ioc = 0, market = 0, cancel = 0;
  };
  Mix below, above;
  for (int i = 0; i < 400'000; ++i) {
    Mix& mix = flow.cancel_candidates() < FlowOptions{}.target_resting ? below : above;
    const Command command = flow.next();
    ++mix.commands;
    if (const auto* order = std::get_if<NewOrder>(&command)) {
      ++(order->type == OrderType::Limit ? mix.limit : order->type == OrderType::Market ? mix.market : mix.ioc);
    } else {
      ++mix.cancel;
    }
  }
  const auto share = [](int part, int whole) { return 100.0 * part / whole; };
  ASSERT_GT(below.commands, 50'000);
  ASSERT_GT(above.commands, 50'000);
  // Each bound is several standard deviations wide; resent orders (about 0.2% of commands) count as new orders.
  EXPECT_NEAR(share(below.limit, below.commands), 60, 1);
  EXPECT_NEAR(share(below.cancel, below.commands), 25, 1);
  EXPECT_NEAR(share(above.limit, above.commands), 35, 1);
  EXPECT_NEAR(share(above.cancel, above.commands), 50, 1);
  for (const Mix* mix : {&below, &above}) {
    EXPECT_NEAR(share(mix->ioc, mix->commands), 10, 1);
    EXPECT_NEAR(share(mix->market, mix->commands), 5, 1);
  }
}

TEST(RandomFlow, ExercisesEveryEventAndEveryRejection) {
  RandomFlow flow(5, FlowOptions{.target_resting = 200});
  ReferenceBook book;
  std::vector<Event> events;
  int accepted = 0, trades = 0, unfilled = 0, requested = 0;
  std::array<int, 4> rejected{};
  std::size_t largest = 0;
  for (int i = 0; i < 50'000; ++i) {
    events.clear();
    book.apply(flow.next(), events);
    for (const Event& event : events) {
      if (std::holds_alternative<Accepted>(event)) ++accepted;
      if (std::holds_alternative<Trade>(event)) ++trades;
      if (const auto* r = std::get_if<Rejected>(&event)) ++rejected[static_cast<std::size_t>(r->reason)];
      if (const auto* c = std::get_if<Cancelled>(&event)) {
        ++(c->reason == CancelReason::Unfilled ? unfilled : requested);
      }
    }
    largest = std::max(largest, book.size());
    // What rests from the stream is bounded by its cancel candidates, which stay near the target.
    ASSERT_LE(book.size(), flow.cancel_candidates()) << "command " << i;
  }
  EXPECT_GT(accepted, 0);
  EXPECT_GT(trades, 0);
  EXPECT_GT(unfilled, 0);
  EXPECT_GT(requested, 0);
  for (std::size_t reason = 0; reason < rejected.size(); ++reason) {
    EXPECT_GT(rejected[reason], 0) << to_string(static_cast<RejectReason>(reason));
  }
  EXPECT_GT(largest, 100U);
  EXPECT_LE(largest, 260U);
}

TEST(RandomFlow, RefusesOptionsItCannotHonour) {
  EXPECT_THROW(RandomFlow(1, FlowOptions{.price_range = -1}), std::invalid_argument);
  EXPECT_THROW(RandomFlow(1, FlowOptions{.price_range = 500'000'000}), std::invalid_argument);
  EXPECT_NO_THROW(RandomFlow(1, FlowOptions{.price_range = 499'999'999}));
  EXPECT_THROW(RandomFlow(1, FlowOptions{.max_quantity = 0}), std::invalid_argument);
  EXPECT_THROW(RandomFlow(1, FlowOptions{.max_quantity = 1'000'000'001}), std::invalid_argument);
  EXPECT_THROW(RandomFlow(1, FlowOptions{.invalid_per_mille = -1}), std::invalid_argument);
  EXPECT_THROW(RandomFlow(1, FlowOptions{.invalid_per_mille = 1001}), std::invalid_argument);
}

// With nothing spoiled, every new order must be accepted, one by one; near either price bound that holds only if the
// mid is kept far enough inside it (the start and every step).
void expect_every_order_accepted(FlowOptions options, int commands) {
  options.invalid_per_mille = 0;
  RandomFlow flow(17, options);
  ReferenceBook book;
  std::vector<Event> events;
  for (int i = 0; i < commands; ++i) {
    const Command command = flow.next();
    events.clear();
    book.apply(command, events);
    if (std::holds_alternative<NewOrder>(command)) {
      ASSERT_TRUE(std::holds_alternative<Accepted>(events.front()))
          << "command " << i << ": '" << format(command) << "' gave '" << format(events.front()) << "'";
    }
  }
}

TEST(RandomFlow, NearTheLowestPriceEveryOrderSentValidIsAccepted) {
  expect_every_order_accepted(FlowOptions{.start_price = 1, .target_resting = 50}, 20'000);
}

TEST(RandomFlow, NearTheHighestPriceEveryOrderSentValidIsAccepted) {
  expect_every_order_accepted(FlowOptions{.start_price = 1'000'000'000, .target_resting = 50}, 20'000);
}

TEST(RandomFlow, ResendsRejectedOrdersFixedAndBreaksTwoRulesAtOnce) {
  // Through the reference model, with many spoiled orders: an order rejected for anything but its id that comes
  // straight back under the same id must now be accepted, since a rejection doesn't use the id up; and some
  // rejected orders break two rules, so the order of the checks decides their reason.
  RandomFlow flow(23, FlowOptions{.target_resting = 100, .invalid_per_mille = 100});
  ReferenceBook book;
  std::vector<Event> events;
  std::optional<OrderId> rejected_id;
  OrderId last_accepted{0};
  int resent = 0, two_rules = 0;
  for (int i = 0; i < 50'000; ++i) {
    const Command command = flow.next();
    events.clear();
    book.apply(command, events);
    const auto* order = std::get_if<NewOrder>(&command);
    if (order == nullptr) continue;
    if (rejected_id && order->id == *rejected_id) {
      ASSERT_TRUE(std::holds_alternative<Accepted>(events.front())) << format(command) << ": " << format(events.front());
      ++resent;
    }
    rejected_id.reset();
    if (const auto* rejected = std::get_if<Rejected>(&events.front())) {
      const bool bad_id = order->id <= last_accepted;
      const bool bad_quantity = !valid_quantity(order->quantity);
      const bool bad_price = order->type == OrderType::Market ? order->price != Price{0} : !valid_price(order->price);
      if (int{bad_id} + int{bad_quantity} + int{bad_price} >= 2) ++two_rules;
      if (rejected->reason != RejectReason::IdNotIncreasing) rejected_id = order->id;
    } else {
      last_accepted = order->id;
    }
  }
  EXPECT_GT(resent, 100);
  EXPECT_GT(two_rules, 100);
}

TEST(RandomFlow, InAFullySpoiledStreamOnlyResendsAreAccepted) {
  // With every new order spoiled, the only orders a correct book accepts are resends. So each accepted order must come
  // straight after the order it resends, under the same id, which was rejected for something other than its id;
  // about half of those rejections must come back; and resent limit orders become cancel candidates (with none, the
  // stream would send no cancels at all), so some cancels succeed. The pinned stream hash would also catch a change
  // here, but re-pinning it is the documented step after a deliberate change, so these properties are checked as such.
  RandomFlow flow(29, FlowOptions{.target_resting = 100, .invalid_per_mille = 1000});
  ReferenceBook book;
  std::vector<Event> events;
  std::optional<OrderId> resendable;  // the previous command's id, if it was a new order rejected for a non-id reason
  int rejections = 0, resent = 0, cancelled = 0;
  for (int i = 0; i < 100'000; ++i) {
    const Command command = flow.next();
    events.clear();
    book.apply(command, events);
    const std::optional<OrderId> previous = resendable;
    resendable.reset();
    if (std::holds_alternative<CancelOrder>(command)) {
      if (std::holds_alternative<Cancelled>(events.front())) ++cancelled;
      continue;
    }
    const NewOrder& order = std::get<NewOrder>(command);
    if (std::holds_alternative<Accepted>(events.front())) {
      ASSERT_TRUE(previous && *previous == order.id) << "command " << i << ", accepted but not a resend: " << format(command);
      ++resent;
    } else if (std::get<Rejected>(events.front()).reason != RejectReason::IdNotIncreasing) {
      resendable = order.id;
      ++rejections;
    }
  }
  ASSERT_GT(rejections, 10'000);
  EXPECT_NEAR(100.0 * resent / rejections, 50.0, 2.0) << resent << " of " << rejections;
  EXPECT_GT(cancelled, 100);
}

TEST(RandomFlow, OrdersSentValidAreAccepted) {
  // The stream spoils about 5 in 1,000 new orders (one in four of those in two fields); every other order must be
  // accepted, so a rejection count far above that rate means the stream sends invalid orders by accident.
  RandomFlow flow(9);
  ReferenceBook book;
  std::vector<Event> events;
  int orders = 0, rejected = 0;
  for (int i = 0; i < 100'000; ++i) {
    const Command command = flow.next();
    events.clear();
    book.apply(command, events);
    if (!std::holds_alternative<NewOrder>(command)) continue;
    ++orders;
    if (std::holds_alternative<Rejected>(events.front())) ++rejected;
  }
  EXPECT_NEAR(rejected, orders * 5 / 1000, orders / 1000);
}

}  // namespace
}  // namespace orderbook
