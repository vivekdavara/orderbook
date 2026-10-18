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
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "orderbook/book.hpp"
#include "orderbook/map_book.hpp"
#include "orderbook/random_flow.hpp"

namespace {

std::atomic<bool> counting{false};
std::atomic<std::size_t> allocations{0};

}  // namespace

void* operator new(std::size_t size) {
  if (counting.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

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
  // The default room (4,096 orders, 1,024 levels a side), on a stream that grows the book past it: the pool and the
  // index double a few times, and nothing else allocates. Then the same stream again, on a book built with room for the
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

}  // namespace
}  // namespace orderbook
