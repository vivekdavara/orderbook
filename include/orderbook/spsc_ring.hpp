#pragma once

// A bounded, lock-free, single-producer single-consumer queue (DESIGN.md, "Threads"): the channel between the
// gateway's thread and the engine's. Exactly one thread pushes and exactly one pops; neither ever waits for the other
// inside the queue, and nothing in it allocates after construction.
//
// - The slots are a power-of-two array. `tail_` counts pushes and `head_` counts pops; each only ever increases, and
//   only its own thread writes it, so a slot's place is the count modulo the size and full means tail - head == size.
// - Publishing is one release store: the producer writes the slot, then stores the new tail with release; the
//   consumer loads the tail with acquire before it reads the slot, so it sees the slot's contents. Freeing is the
//   mirror image: the consumer reads the slot, then stores the new head with release, and the producer loads the head
//   with acquire before it overwrites the slot.
// - Each side keeps a private copy of the other's index and loads the shared one only when its copy says the queue
//   is full (the producer) or empty (the consumer), so in the common case a push or pop touches no line the other
//   thread writes, apart from the slot itself.
// - The two indices are on separate cache lines, each with its own side's copy of the other: 128 bytes, the line
//   size of Apple's M-series and the pair a modern x86 prefetches together, so the two threads never write one line.

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace orderbook {

inline constexpr std::size_t kCacheLine = 128;

template <typename T, std::size_t Size>
class SpscRing {
  static_assert(Size >= 2 && (Size & (Size - 1)) == 0, "the size must be a power of two");
  static_assert(std::is_default_constructible_v<T> && std::is_copy_assignable_v<T>);

 public:
  SpscRing() = default;
  SpscRing(const SpscRing&) = delete;  // the threads share one ring by reference
  SpscRing& operator=(const SpscRing&) = delete;

  // The producer's side. Copies `value` into the next slot and returns true, or returns false if the ring is full.
  bool try_push(const T& value) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);  // only this thread writes it
    if (tail - head_seen_ == Size) {
      head_seen_ = head_.load(std::memory_order_acquire);
      if (tail - head_seen_ == Size) return false;
    }
    slots_[tail & (Size - 1)] = value;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // The consumer's side. Copies the oldest value into `out` and returns true, or returns false if the ring is empty.
  bool try_pop(T& out) {
    const std::size_t head = head_.load(std::memory_order_relaxed);  // only this thread writes it
    if (head == tail_seen_) {
      tail_seen_ = tail_.load(std::memory_order_acquire);
      if (head == tail_seen_) return false;
    }
    out = slots_[head & (Size - 1)];
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // How many values are in the ring. Either thread may ask; while both run, it is a recent count, not an exact one.
  std::size_t size() const {
    const std::size_t head = head_.load(std::memory_order_acquire);
    return tail_.load(std::memory_order_acquire) - head;
  }
  static constexpr std::size_t capacity() { return Size; }

 private:
  // The producer's line: what it writes, and its copy of the consumer's index.
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  std::size_t head_seen_ = 0;
  // The consumer's line.
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  std::size_t tail_seen_ = 0;
  alignas(kCacheLine) std::array<T, Size> slots_{};
};

}  // namespace orderbook
