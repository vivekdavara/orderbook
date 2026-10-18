// The single-producer single-consumer ring: its full and empty states and order on one thread, then a producer and
// a consumer on two threads, on rings small enough that both sides keep finding it full or empty.
//
// `make tsan` runs these under ThreadSanitizer, which reports a missing happens-before between writing a slot and
// reading it as a data race, but only for values copied word by word: on this Mac it reported a ring of integers whose
// push published with a relaxed store, and not a ring of 64-byte structs with the same bug, which are copied with
// memcpy. So the two-thread tests run on both: integers for TSan, and structs whose fields must agree, which catch a
// slot read before it was written on hardware that reorders (a relaxed load of the tail failed them on an M3).

#include "orderbook/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace orderbook {
namespace {

TEST(SpscRing, IsEmptyThenFullThenEmptyAgain) {
  SpscRing<int, 4> ring;
  int out = 0;
  EXPECT_FALSE(ring.try_pop(out));
  EXPECT_EQ(ring.size(), 0U);
  for (int i = 1; i <= 4; ++i) EXPECT_TRUE(ring.try_push(i)) << i;
  EXPECT_FALSE(ring.try_push(5)) << "a ring of 4 holds 4";
  EXPECT_EQ(ring.size(), 4U);
  for (int i = 1; i <= 4; ++i) {
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, i);
  }
  EXPECT_FALSE(ring.try_pop(out));
  EXPECT_TRUE(ring.try_push(6)) << "room again once a value is popped";
  ASSERT_TRUE(ring.try_pop(out));
  EXPECT_EQ(out, 6);
}

TEST(SpscRing, KeepsOrderAcrossManyLapsOfItsSlots) {
  SpscRing<std::uint64_t, 8> ring;
  std::uint64_t pushed = 0;
  std::uint64_t popped = 0;
  // Fill to a varying depth, then drain part of it, for many laps: every slot is reused in every position.
  for (std::uint64_t round = 0; round < 10'000; ++round) {
    for (std::uint64_t i = 0; i < 1 + round % 8; ++i) {
      if (!ring.try_push(pushed)) break;
      ++pushed;
    }
    for (std::uint64_t i = 0; i < 1 + (round * 7) % 8; ++i) {
      std::uint64_t out = 0;
      if (!ring.try_pop(out)) break;
      ASSERT_EQ(out, popped) << "round " << round;
      ++popped;
    }
    ASSERT_EQ(ring.size(), pushed - popped);
    ASSERT_LE(ring.size(), 8U);
  }
  EXPECT_GT(pushed, 8U * 1'000U) << "at least 1,000 laps of the 8 slots";  // 37,500 values when written
}

// A value big enough to span many words, with fields that must agree: a slot read before its write was published,
// or overwritten while being read, shows up as a value whose fields don't.
struct Message {
  std::uint64_t sequence = 0;
  std::uint64_t words[6] = {};
  std::uint64_t check = 0;
};

Message message(std::uint64_t sequence) {
  Message m;
  m.sequence = sequence;
  for (std::uint64_t i = 0; i < 6; ++i) m.words[i] = sequence * 31 + i;
  m.check = ~sequence;
  return m;
}

bool whole(const Message& m) {
  for (std::uint64_t i = 0; i < 6; ++i) {
    if (m.words[i] != m.sequence * 31 + i) return false;
  }
  return m.check == ~m.sequence;
}

// The integers' versions of the same.
std::uint64_t message(std::uint64_t sequence, std::uint64_t /*tag*/) { return sequence; }
bool whole(std::uint64_t) { return true; }
std::uint64_t sequence_of(std::uint64_t value) { return value; }
Message message(std::uint64_t sequence, Message /*tag*/) { return message(sequence); }
std::uint64_t sequence_of(const Message& m) { return m.sequence; }

// One thread pushes 0, 1, 2, ... and another pops; the consumer must see exactly that sequence, every value whole.
template <typename Value, std::size_t Size>
void two_threads(std::uint64_t count) {
  auto ring = std::make_unique<SpscRing<Value, Size>>();
  std::thread producer([&ring, count] {
    for (std::uint64_t sequence = 0; sequence < count; ++sequence) {
      const Value value = message(sequence, Value{});
      while (!ring->try_push(value)) std::this_thread::yield();
    }
  });
  std::uint64_t expected = 0;
  std::uint64_t broken = 0;
  std::uint64_t out_of_order = 0;
  while (expected < count) {
    Value value{};
    if (!ring->try_pop(value)) {
      std::this_thread::yield();
      continue;
    }
    if (!whole(value)) ++broken;
    if (sequence_of(value) != expected) ++out_of_order;
    ++expected;
  }
  producer.join();
  EXPECT_EQ(broken, 0U);
  EXPECT_EQ(out_of_order, 0U);
  Value extra{};
  EXPECT_FALSE(ring->try_pop(extra)) << "nothing more than was pushed";
}

TEST(SpscRing, TwoThreadsOnASmallRingSeeEveryValueWholeAndInOrder) { two_threads<Message, 4>(300'000); }

TEST(SpscRing, TwoThreadsOnALargerRingSeeEveryValueWholeAndInOrder) { two_threads<Message, 1024>(1'000'000); }

TEST(SpscRing, TwoThreadsOnASmallRingSeeEveryIntegerInOrder) { two_threads<std::uint64_t, 4>(300'000); }

TEST(SpscRing, TwoThreadsOnALargerRingSeeEveryIntegerInOrder) { two_threads<std::uint64_t, 1024>(1'000'000); }

}  // namespace
}  // namespace orderbook
