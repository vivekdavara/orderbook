// The engine's id index on its own: lookups, inserts and removals against std::unordered_map as the model, on tables
// small enough that clusters form, wrap around the end of the array and are cut by removals in every position.

#include "orderbook/id_table.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>


namespace orderbook {
namespace {

static_assert(std::is_nothrow_move_constructible_v<IdTable> && std::is_nothrow_move_assignable_v<IdTable>);

// A seeded stream of numbers for the random tests (SplitMix64, as RandomFlow uses).
class Numbers {
 public:
  explicit Numbers(std::uint64_t seed) : state_(seed) {}
  std::uint64_t next() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  std::uint64_t below(std::uint64_t n) { return next() % n; }  // slightly biased; fine for a test's choices

 private:
  std::uint64_t state_;
};

TEST(IdTable, FindsWhatWasInsertedAndNothingElse) {
  IdTable table(8);
  EXPECT_EQ(table.find(1), kNoNode);
  table.insert(1, 10);
  table.insert(2, 20);
  table.insert(1'000'000'007, 30);
  EXPECT_EQ(table.size(), 3U);
  EXPECT_EQ(table.find(1), 10U);
  EXPECT_EQ(table.find(2), 20U);
  EXPECT_EQ(table.find(1'000'000'007), 30U);
  EXPECT_EQ(table.find(3), kNoNode);
  // Id 0 marks an empty slot, so it is never found, even in a table of empty slots, and taking it changes nothing
  // (without its guard, take(0) stopped at the first empty slot, called it id 0's, and counted one id fewer).
  EXPECT_EQ(table.find(0), kNoNode);
  EXPECT_EQ(table.take(0), kNoNode);
  EXPECT_EQ(table.size(), 3U);
  EXPECT_EQ(table.find(1'000'000'007), 30U);
}

TEST(IdTable, TakeRemovesAndReturnsTheNode) {
  IdTable table(8);
  table.insert(5, 50);
  table.insert(6, 60);
  EXPECT_EQ(table.take(5), 50U);
  EXPECT_EQ(table.size(), 1U);
  EXPECT_EQ(table.find(5), kNoNode);
  EXPECT_EQ(table.take(5), kNoNode);  // already gone
  EXPECT_EQ(table.take(7), kNoNode);  // never there
  EXPECT_EQ(table.find(6), 60U);
  EXPECT_EQ(table.size(), 1U);
}

TEST(IdTable, IsAtMostHalfFullAndGrowsByDoubling) {
  IdTable table(100);
  EXPECT_EQ(table.capacity(), 256U);  // the power of two at or above twice the size asked for
  for (std::uint64_t id = 1; id <= 128; ++id) table.insert(id, static_cast<NodeIndex>(id));
  EXPECT_EQ(table.capacity(), 256U);  // exactly half full: no growth yet
  table.insert(129, 129);
  EXPECT_EQ(table.capacity(), 512U);
  for (std::uint64_t id = 1; id <= 129; ++id) ASSERT_EQ(table.find(id), id) << id;
}

TEST(IdTable, AnEmptyTableHasNoSlotsUntilItsFirstInsert) {
  IdTable table;
  EXPECT_EQ(table.capacity(), 0U);
  EXPECT_EQ(table.find(1), kNoNode);
  EXPECT_EQ(table.take(1), kNoNode);
  table.insert(1, 7);
  EXPECT_EQ(table.capacity(), 16U);
  EXPECT_EQ(table.find(1), 7U);
}

TEST(IdTable, AMovedFromTableIsEmptyAndWorksAgain) {
  IdTable table(8);
  table.insert(1, 1);
  table.insert(2, 2);
  IdTable moved(std::move(table));
  EXPECT_EQ(moved.find(2), 2U);
  EXPECT_EQ(table.size(), 0U);      // a moved-from table is empty
  EXPECT_EQ(table.capacity(), 0U);
  EXPECT_EQ(table.find(1), kNoNode);
  table.insert(3, 3);
  EXPECT_EQ(table.find(3), 3U);
  EXPECT_EQ(moved.find(3), kNoNode);

  IdTable assigned(8);
  assigned.insert(9, 9);
  assigned = std::move(moved);
  EXPECT_EQ(assigned.find(1), 1U);
  EXPECT_EQ(assigned.find(9), kNoNode);
  EXPECT_EQ(moved.size(), 0U);
  EXPECT_EQ(moved.capacity(), 0U);
  moved.insert(4, 4);
  EXPECT_EQ(moved.find(4), 4U);
}

TEST(IdTable, ACopyIsATableOfItsOwn) {
  IdTable table(8);
  table.insert(1, 1);
  IdTable copy = table;
  copy.insert(2, 2);
  EXPECT_EQ(copy.take(1), 1U);
  EXPECT_EQ(table.find(1), 1U);
  EXPECT_EQ(table.find(2), kNoNode);
}

// Random inserts and removals against the model, with every stored id looked up after every removal, on the
// smallest table (16 slots, so 8 ids at most before it grows) and on larger ones. At half full, linear probing makes
// clusters of several slots, which wrap past the end of the array; a removal from any place in one must leave every
// other id of the cluster findable (Algorithm R's rule for which entries move back).
void random_operations(std::uint64_t seed, std::size_t live, std::size_t operations) {
  Numbers numbers(seed);
  IdTable table(live);
  const std::size_t room = table.capacity();
  std::unordered_map<std::uint64_t, NodeIndex> model;
  std::vector<std::uint64_t> stored;
  std::uint64_t next_id = 1;
  for (std::size_t i = 0; i < operations; ++i) {
    const bool insert = stored.size() < live && (stored.empty() || numbers.below(2) == 0);
    if (insert) {
      // Ids increase, as the book's do, with gaps (orders that never rested).
      next_id += 1 + numbers.below(3);
      const auto node = static_cast<NodeIndex>(numbers.below(1'000'000));
      table.insert(next_id, node);
      model.emplace(next_id, node);
      stored.push_back(next_id);
    } else {
      const std::size_t pick = numbers.below(stored.size());
      const std::uint64_t id = stored[pick];
      stored[pick] = stored.back();
      stored.pop_back();
      ASSERT_EQ(table.take(id), model.at(id)) << "seed " << seed << ", operation " << i;
      model.erase(id);
      ASSERT_EQ(table.find(id), kNoNode) << "seed " << seed << ", operation " << i;
      for (const auto& [kept, node] : model) {
        ASSERT_EQ(table.find(kept), node) << "seed " << seed << ", operation " << i << ", id " << kept;
      }
    }
    ASSERT_EQ(table.size(), model.size());
  }
  EXPECT_EQ(table.capacity(), room) << "never more than `live` ids stored, so it never grew";
}

TEST(IdTable, MatchesAMapOnRandomOperationsInTheSmallestTable) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) random_operations(seed, 8, 5'000);
}

TEST(IdTable, MatchesAMapOnRandomOperationsInLargerTables) {
  random_operations(101, 64, 20'000);
  random_operations(102, 500, 20'000);
}

// Growth from nothing while ids come and go: every seventh id inserted, and every third insert taking an older one
// out, as orders rest and leave, with the table doubling under clusters it already has.
TEST(IdTable, GrowsFromNothingWithEveryIdKept) {
  IdTable table;
  std::unordered_map<std::uint64_t, NodeIndex> model;
  for (std::uint64_t id = 1; id <= 50'000; ++id) {
    table.insert(id * 7, static_cast<NodeIndex>(id));
    model.emplace(id * 7, static_cast<NodeIndex>(id));
    if (id % 3 == 0) {
      ASSERT_EQ(table.take(id * 7 - 14), model.at(id * 7 - 14));
      model.erase(id * 7 - 14);
    }
  }
  EXPECT_EQ(table.size(), model.size());
  EXPECT_GE(table.capacity(), 2 * table.size());
  for (const auto& [id, node] : model) ASSERT_EQ(table.find(id), node) << id;
}

TEST(IdTable, MovingATableIntoItselfChangesNothing) {
  IdTable table(8);
  table.insert(1, 1);
  IdTable& same = table;
  table = std::move(same);
  EXPECT_EQ(table.size(), 1U);
  EXPECT_EQ(table.find(1), 1U);
}

}  // namespace
}  // namespace orderbook
