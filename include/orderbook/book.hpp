#pragma once

// The matching engine (DESIGN.md, "The engine"): the rules of DESIGN.md, with each operation's cost set by where the
// order sits rather than by how many orders rest, and no allocation once the book is as big as it was built for.
// Day 3's design, replacing day 2's standard containers (kept as `MapBook` in the test kit, the baseline):
//
// - Orders are nodes in a pool: one vector of nodes, reused through a free list, linked by 32-bit indices rather than
//   pointers. Each price level is an intrusive FIFO of those nodes (head, tail, and each node's prev and next), oldest
//   first, with the level's total quantity kept as orders fill and cancel.
// - Each side is a vector of its levels sorted worst first, so the best level is the back: a taker trades from the
//   back and pops a level it empties; a new level near the top of the book is inserted near the back, moving few.
// - An open-addressing table from order id to node (`IdTable`) finds the order a cancel names in one probe on
//   average; the cancel then finds its level by a binary search on price.
//
// It answers every command with exactly the events the reference model gives, and reports its state through the
// same queries; the differential tests compare the two, event for event, on long random streams.

#include <cstddef>
#include <optional>
#include <vector>

#include "orderbook/command.hpp"
#include "orderbook/event.hpp"
#include "orderbook/id_table.hpp"
#include "orderbook/resting_order.hpp"
#include "orderbook/text.hpp"
#include "orderbook/types.hpp"

namespace orderbook {

// The room a book makes when it is built: resting orders on both sides, and price levels on each side. Within it,
// nothing the book does allocates. Past it the book still works: it grows, allocating each time it passes a new
// high-water mark (the pool and the index double; a side's levels grow as a vector does).
struct BookCapacity {
  std::size_t orders = 4'096;
  std::size_t levels = 1'024;
};

class Book {
 public:
  Book() : Book(BookCapacity{}) {}
  explicit Book(BookCapacity capacity);
  // Copyable and movable member by member: nodes are linked by index into the book's own pool, so a copy is a book
  // of its own and a move keeps every link. A moved-from book is empty, with no room made, and can be used again.
  Book(const Book&) = default;
  Book& operator=(const Book&) = default;
  Book(Book&& other) noexcept;
  Book& operator=(Book&& other) noexcept;
  ~Book() = default;

  // Applies one command and appends the events it produces to `events`.
  void apply(const Command& command, std::vector<Event>& events);

  // The best resting price on a side (the highest bid or the lowest ask), if the side has any orders. O(1).
  std::optional<Price> best_price(Side side) const;
  // A side's price levels, best first. O(levels).
  std::vector<Level> depth(Side side) const;
  // A side's resting orders in priority order: best price first, then arrival. O(orders).
  std::vector<RestingOrder> orders(Side side) const;
  // Resting orders on both sides.
  std::size_t size() const { return index_.size(); }
  // The highest id accepted so far (0 before any): a new order's id must be greater.
  OrderId last_accepted_id() const { return last_accepted_; }
  // Nodes the pool has made: the most orders that have rested at once (or the room made, if more). For the tests.
  std::size_t pool_nodes() const { return nodes_.size(); }

 private:
  struct OrderNode {
    OrderId id;
    Quantity remaining;
    Price price;  // to find its level when cancelled
    NodeIndex prev = kNoNode;  // the order ahead of it at its price, or kNoNode at the front; in the free list, unused
    NodeIndex next = kNoNode;  // the order behind it, or kNoNode at the back; in the free list, the next free node
    Side side = Side::Buy;
  };
  struct PriceLevel {
    Price price;
    Quantity total;            // the queue's remaining quantities summed, kept as they change
    NodeIndex head = kNoNode;  // the oldest order: next to trade
    NodeIndex tail = kNoNode;  // the newest
  };
  // A side's levels, worst first: the best level is the back. Bids go up in price, asks down.
  using Ladder = std::vector<PriceLevel>;

  void new_order(const NewOrder& order, std::vector<Event>& events);
  void cancel(const CancelOrder& cancel, std::vector<Event>& events);
  std::optional<RejectReason> validate(const NewOrder& order) const;
  void rest(const NewOrder& order, Quantity remaining);

  // The level at `price` on `side` if there is one, else where to insert it: the first level not worse than `price`.
  static Ladder::iterator find_level(Ladder& levels, Side side, Price price);
  NodeIndex allocate();
  void release(NodeIndex node);

  Ladder& ladder(Side side) { return side == Side::Buy ? bids_ : asks_; }
  const Ladder& ladder(Side side) const { return side == Side::Buy ? bids_ : asks_; }

  std::vector<OrderNode> nodes_;  // the pool: grows only past its high-water mark
  NodeIndex free_ = kNoNode;      // the first free node, linked through `next`
  Ladder bids_;
  Ladder asks_;
  IdTable index_;  // every resting order, by id
  OrderId last_accepted_{0};
};

}  // namespace orderbook
