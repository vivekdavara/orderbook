// The engine's promise that matching allocates nothing once the book has room (DESIGN.md, "The engine"), checked by
// counting calls to the global operator new, which this test binary replaces. Day 2's engine on the same commands is
// the control: it allocates for every order that rests, so a counter that missed the containers' allocations would
// fail its test.
//
// This is its own executable, since the replacement is process-wide, and it isn't built under the sanitizers, which
// replace operator new themselves.

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <variant>
#include <string>
#include <vector>

#include "orderbook/book.hpp"
#include "orderbook/check.hpp"
#include "orderbook/map_book.hpp"
#include "orderbook/random_flow.hpp"

namespace {

std::atomic<bool> counting{false};
std::atomic<std::size_t> allocations{0};
// How many allocations to let through before one fails with bad_alloc (once); -1 for none. One thread uses it.
std::atomic<long> allocations_before_failure{-1};

}  // namespace

// GCC (14 and 15) inlines these into callers and then reports the replacement operator delete's std::free as freeing
// "a pointer returned from a mismatched allocation function" (-Wmismatched-new-delete): it pairs a new-expression with
// free, not knowing that this operator new is malloc underneath. They are matched here by construction.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(std::size_t size) {
  if (counting.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
  const long left = allocations_before_failure.load(std::memory_order_relaxed);
  if (left >= 0) {
    allocations_before_failure.store(left - 1, std::memory_order_relaxed);
    if (left == 0) throw std::bad_alloc();
  }
  if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace orderbook {
namespace {

// The allocations made while `work` runs. Nothing inside it may use GoogleTest's assertions, which allocate.
template <typename Work>
std::size_t allocations_during(Work&& work) {
  allocations.store(0);
  counting.store(true);
  work();
  counting.store(false);
  return allocations.load();
}

std::vector<Command> stream(std::uint64_t seed, FlowOptions options, std::size_t count) {
  RandomFlow flow(seed, options);
  std::vector<Command> commands;
  commands.reserve(count);
  for (std::size_t i = 0; i < count; ++i) commands.push_back(flow.next());
  return commands;
}

// Applies every command, reusing one events vector that already has room for the longest sweep these streams make.
template <typename AnyBook>
std::size_t allocations_applying(AnyBook& book, const std::vector<Command>& commands) {
  std::vector<Event> events;
  events.reserve(1 << 16);
  return allocations_during([&] {
    for (const Command& command : commands) {
      events.clear();
      book.apply(command, events);
    }
  });
}

TEST(Allocation, TheCounterCountsCallsToOperatorNew) {
  static void* volatile kept = nullptr;  // escapes, so the call can't be optimised away
  EXPECT_EQ(allocations_during([] {
              kept = ::operator new(16);
              ::operator delete(kept);
            }),
            1U);
  EXPECT_EQ(allocations_during([] { kept = nullptr; }), 0U);
}

struct Mix {
  const char* name;
  std::uint64_t seed;
  FlowOptions flow;
};

const Mix kMixes[] = {
    {"default", 1, FlowOptions{}},
    {"deep book", 2, FlowOptions{.price_range = 2'000, .target_resting = 5'000}},
    {"one-lot orders", 3, FlowOptions{.price_range = 5, .max_quantity = 1, .target_resting = 50}},
    {"many invalid orders", 4, FlowOptions{.target_resting = 100, .invalid_per_mille = 300}},
};

TEST(Allocation, TheEngineMakesNoneWithinTheRoomItWasBuiltWith) {
  for (const Mix& mix : kMixes) {
    const std::vector<Command> commands = stream(mix.seed, mix.flow, 200'000);
    Book book(BookCapacity{.orders = 20'000, .levels = 8'000});
    EXPECT_EQ(allocations_applying(book, commands), 0U) << mix.name;
    EXPECT_LE(book.pool_nodes(), 20'000U) << mix.name;  // it never needed more room than it was built with
    EXPECT_GT(book.pool_nodes(), mix.flow.target_resting / 2) << mix.name << ": the stream built a book";
  }
}

TEST(Allocation, Day2sEngineAllocatesForEveryOrderThatRests) {
  // The control: the same streams through day 2's engine. Each order that rests allocates a list node and an index
  // node, and each new level a map node, so the count is at least the pool's high-water mark and in practice far
  // more, since freed nodes go back to the allocator.
  for (const Mix& mix : kMixes) {
    const std::vector<Command> commands = stream(mix.seed, mix.flow, 200'000);
    MapBook map_book;
    Book book;
    const std::size_t counted = allocations_applying(map_book, commands);
    const std::size_t engine = allocations_applying(book, commands);
    std::printf("%s: day 2's engine %zu allocations, the engine %zu (pool %zu nodes)\n", mix.name, counted, engine,
                book.pool_nodes());
    EXPECT_GE(counted, 2 * book.pool_nodes()) << mix.name;
    EXPECT_GT(counted, 10'000U) << mix.name;
  }
}

TEST(Allocation, PastItsRoomTheEngineAllocatesOnlyAtNewHighWaterMarks) {
  // The default room (4,096 orders, 1,024 levels a side), on a stream that grows the book past it: the pool, the index
  // and each side's levels grow, once each here, and nothing else allocates. Then the same stream again, on a book built with room for the
  // high-water mark the first reached, makes no allocation at all.
  const std::vector<Command> commands =
      stream(5, FlowOptions{.price_range = 3'000, .target_resting = 12'000}, 100'000);
  Book book;
  const std::size_t growing = allocations_applying(book, commands);
  EXPECT_GT(book.pool_nodes(), 4'096U) << "the stream grew the book past its room";
  EXPECT_GT(growing, 0U);
  EXPECT_LE(growing, 12U) << "a few doublings of the pool, the index and the levels";
  std::printf("past its room: %zu allocations in 100,000 commands, to a pool of %zu nodes\n", growing,
              book.pool_nodes());

  Book fresh(BookCapacity{.orders = book.pool_nodes(), .levels = 8'000});
  EXPECT_EQ(allocations_applying(fresh, commands), 0U);
}

TEST(Allocation, ACopyKeepsTheRoomItsOriginalWasBuiltWith) {
  // A vector's copy has room only for what it holds; a book's copy gets the room its original had.
  const std::vector<Command> commands = stream(6, FlowOptions{.price_range = 2'000, .target_resting = 5'000}, 200'000);
  const Book empty(BookCapacity{.orders = 20'000, .levels = 8'000});
  Book copy = empty;
  EXPECT_EQ(allocations_applying(copy, commands), 0U) << "a copy of an empty book with room";

  Book used(BookCapacity{.orders = 20'000, .levels = 8'000});
  const std::vector<Command> first(commands.begin(), commands.begin() + 100'000);
  const std::vector<Command> second(commands.begin() + 100'000, commands.end());
  allocations_applying(used, first);
  Book assigned;
  assigned = used;
  EXPECT_EQ(allocations_applying(assigned, second), 0U) << "a copy assigned from a book in use";
}

// The book stays well-formed whatever allocation fails: its own (the pool, a side's levels, the index growing past
// the room) or the caller's events vector growing. Each command of a random stream is applied to a copy of the book,
// failing its first allocation, then its second, and so on until it goes through. After each failure the copy must be
// well-formed (check_book), hold as many orders as its index says, find every one of them for a cancel, and its events
// must be the start of what the command gives when nothing fails.
TEST(Allocation, AFailedAllocationLeavesAWellFormedBook) {
  const std::vector<Command> commands = stream(7, FlowOptions{.price_range = 20, .target_resting = 60}, 3'000);
  Book book(BookCapacity{.orders = 1, .levels = 1});  // so the pool, the levels and the index all grow
  std::size_t failures = 0;
  for (std::size_t i = 0; i < commands.size(); ++i) {
    std::vector<Event> expected;
    {
      Book copy = book;
      copy.apply(commands[i], expected);
    }
    for (long fail_at = 0;; ++fail_at) {
      Book copy = book;
      std::vector<Event> events;  // empty, so its growth can fail too
      allocations_before_failure.store(fail_at);
      bool threw = false;
      try {
        copy.apply(commands[i], events);
      } catch (const std::bad_alloc&) {
        threw = true;
      }
      allocations_before_failure.store(-1);
      if (!threw) break;
      ++failures;
      const std::string where = "command " + std::to_string(i) + ", allocation " + std::to_string(fail_at);
      ASSERT_LE(events.size(), expected.size()) << where;
      ASSERT_TRUE(std::equal(events.begin(), events.end(), expected.begin())) << where;
      ASSERT_TRUE(check_book(copy).empty()) << where << ": " << check_book(copy).front();
      // An order counts as accepted only once its acceptance is reported.
      const bool accepted = !events.empty() && std::holds_alternative<Accepted>(events.front());
      ASSERT_EQ(copy.last_accepted_id(), accepted ? std::get<Accepted>(events.front()).id : book.last_accepted_id())
          << where;
      std::vector<RestingOrder> resting = copy.orders(Side::Buy);
      for (const RestingOrder& order : copy.orders(Side::Sell)) resting.push_back(order);
      ASSERT_EQ(copy.size(), resting.size()) << where;
      for (const RestingOrder& order : resting) {
        std::vector<Event> cancelled;
        copy.apply(CancelOrder{order.id}, cancelled);
        ASSERT_EQ(cancelled.size(), 1U) << where;
        ASSERT_TRUE(std::holds_alternative<Cancelled>(cancelled.front())) << where << ": order " << order.id.value;
      }
      ASSERT_EQ(copy.size(), 0U) << where;
    }
    std::vector<Event> events;
    book.apply(commands[i], events);
  }
  EXPECT_GT(failures, 3'000U) << "every command failed at least at its first event";
  std::printf("%zu commands, %zu failed allocations\n", commands.size(), failures);
}

}  // namespace
}  // namespace orderbook
