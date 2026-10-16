#include "orderbook/random_flow.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
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
  EXPECT_EQ(hash_stream(flow, 100'000), 0x2c53c79c96b493e7ULL);
}

TEST(RandomFlow, EveryCommandHasATextFormThatParsesBack) {
  RandomFlow flow(3);
  for (int i = 0; i < 20'000; ++i) {
    const Command command = flow.next();
    ASSERT_EQ(parse_command(format(command)), command) << format(command);
  }
}

TEST(RandomFlow, MixesCommandsAsDocumented) {
  RandomFlow flow(11);
  std::array<int, 3> types{};  // limit, ioc, market
  int cancels = 0;
  constexpr int kCount = 200'000;
  for (int i = 0; i < kCount; ++i) {
    const Command command = flow.next();
    if (const auto* order = std::get_if<NewOrder>(&command)) {
      ++types[static_cast<std::size_t>(order->type)];
    } else {
      ++cancels;
    }
  }
  // IOC and market orders are a fixed 10% and 5%; limits and cancels share the rest, about evenly once the stream
  // is past its target. Each bound is several standard deviations wide.
  EXPECT_NEAR(types[1], kCount / 10, kCount / 100);
  EXPECT_NEAR(types[2], kCount / 20, kCount / 100);
  EXPECT_NEAR(types[0] + cancels, kCount * 85 / 100, kCount / 100);
  EXPECT_GT(cancels, kCount / 4);
  EXPECT_GT(types[0], kCount / 4);
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

TEST(RandomFlow, OrdersSentValidAreAccepted) {
  // The stream spoils about 5 in 1,000 new orders, each in exactly one field; every other order must be accepted,
  // so a rejection count far above that rate means the stream sends invalid orders by accident.
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
