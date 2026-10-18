#pragma once

// The engine's index from order id to the order's node in its pool (DESIGN.md, "The engine"): an open-addressing hash
// table. One array of 16-byte slots, linear probing, at most half full; Fibonacci hashing spreads the increasing ids
// across it; deletion shifts later entries of the cluster back (Knuth's Algorithm R), so there are no tombstones and
// every probe stops at the first empty slot.
//
// Id 0 marks an empty slot. The book never stores it: ids start at 1. The table grows (doubling, rehashing every id)
// only when an insert would take it past half full, so a book that stays within the size it was built for never
// allocates here.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace orderbook {

// A node's place in the engine's pool, which links its nodes by index rather than by pointer.
using NodeIndex = std::uint32_t;
inline constexpr NodeIndex kNoNode = std::numeric_limits<NodeIndex>::max();

class IdTable {
 public:
  // Room for `expected` ids before the first growth; none at all for 0, so the first insert allocates.
  explicit IdTable(std::size_t expected = 0) {
    if (expected > 0) resize(std::bit_ceil(std::max(kMinSlots, expected * 2)));
  }
  IdTable(const IdTable&) = default;
  IdTable& operator=(const IdTable&) = default;
  // A moved-from table is empty, with no slots, and works again from its next insert.
  IdTable(IdTable&& other) noexcept
      : slots_(std::move(other.slots_)),
        mask_(std::exchange(other.mask_, 0)),
        shift_(std::exchange(other.shift_, 0)),
        size_(std::exchange(other.size_, 0)) {}
  IdTable& operator=(IdTable&& other) noexcept {
    if (this != &other) {
      slots_ = std::move(other.slots_);
      other.slots_.clear();  // std::vector's move assignment leaves the source valid but unspecified
      mask_ = std::exchange(other.mask_, 0);
      shift_ = std::exchange(other.shift_, 0);
      size_ = std::exchange(other.size_, 0);
    }
    return *this;
  }
  ~IdTable() = default;

  // The node stored for `id`, or kNoNode.
  NodeIndex find(std::uint64_t id) const {
    if (size_ == 0 || id == kEmpty) return kNoNode;
    for (std::size_t slot = home(id);; slot = (slot + 1) & mask_) {
      if (slots_[slot].id == id) return slots_[slot].node;
      if (slots_[slot].id == kEmpty) return kNoNode;  // never runs off: at least half the slots are empty
    }
  }

  // Stores `node` for `id`. The id must not be 0 or stored already.
  void insert(std::uint64_t id, NodeIndex node) {
    if ((size_ + 1) * 2 > slots_.size()) resize(slots_.empty() ? kMinSlots : slots_.size() * 2);
    place(id, node);
    ++size_;
  }

  // Removes `id` and returns its node, or returns kNoNode if it isn't stored.
  NodeIndex take(std::uint64_t id) {
    if (size_ == 0 || id == kEmpty) return kNoNode;
    std::size_t hole = home(id);
    while (slots_[hole].id != id) {
      if (slots_[hole].id == kEmpty) return kNoNode;
      hole = (hole + 1) & mask_;
    }
    const NodeIndex node = slots_[hole].node;
    // Algorithm R: walk the rest of the cluster; an entry whose probe started at or before the hole (cyclically) can
    // move back into it, and its old slot becomes the hole. One whose home lies after the hole, up to its own slot,
    // has to stay: moved back, its probe would start after it and miss it.
    for (std::size_t next = (hole + 1) & mask_; slots_[next].id != kEmpty; next = (next + 1) & mask_) {
      const std::size_t start = home(slots_[next].id);
      const bool stays = hole <= next ? hole < start && start <= next : hole < start || start <= next;
      if (stays) continue;
      slots_[hole] = slots_[next];
      hole = next;
    }
    slots_[hole] = Slot{};
    --size_;
    return node;
  }

  std::size_t size() const { return size_; }
  // Slots in the array: 0, or a power of two at least twice size().
  std::size_t capacity() const { return slots_.size(); }

 private:
  static constexpr std::uint64_t kEmpty = 0;
  static constexpr std::size_t kMinSlots = 16;
  struct Slot {
    std::uint64_t id = kEmpty;
    NodeIndex node = kNoNode;
  };

  // Where `id`'s probe starts: the top bits of id times 2^64 divided by the golden ratio (Fibonacci hashing), which
  // sends consecutive ids far apart.
  std::size_t home(std::uint64_t id) const { return (id * 0x9E3779B97F4A7C15ULL) >> shift_; }

  void place(std::uint64_t id, NodeIndex node) {
    std::size_t slot = home(id);
    while (slots_[slot].id != kEmpty) slot = (slot + 1) & mask_;
    slots_[slot] = Slot{id, node};
  }

  // A new array of `slots` (a power of two of at least kMinSlots), with every stored id placed again.
  void resize(std::size_t slots) {
    std::vector<Slot> old = std::exchange(slots_, std::vector<Slot>(slots));
    mask_ = slots - 1;
    shift_ = static_cast<unsigned>(std::numeric_limits<std::uint64_t>::digits - std::countr_zero(slots));
    for (const Slot& slot : old) {
      if (slot.id != kEmpty) place(slot.id, slot.node);
    }
  }

  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  unsigned shift_ = 0;  // 64 - log2(slots); used only once there are slots
  std::size_t size_ = 0;
};

}  // namespace orderbook
