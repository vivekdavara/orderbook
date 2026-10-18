#include "orderbook/book.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <variant>

namespace orderbook {
namespace {

// Matching rule 1: whether the taker crosses a maker resting at `maker_price`.
bool crosses(const NewOrder& taker, Price maker_price) {
  if (taker.type == OrderType::Market) return true;
  return taker.side == Side::Buy ? maker_price <= taker.price : maker_price >= taker.price;
}

}  // namespace

Book::Book(BookCapacity capacity) : index_(capacity.orders) {
  static_assert(sizeof(OrderNode) == 40 && sizeof(PriceLevel) == 24, "the sizes DESIGN.md gives");
  nodes_.reserve(capacity.orders);
  bids_.reserve(capacity.levels);
  asks_.reserve(capacity.levels);
}

// std::vector's move constructor leaves the source empty; the scalars are reset by hand.
Book::Book(Book&& other) noexcept
    : nodes_(std::move(other.nodes_)),
      free_(std::exchange(other.free_, kNoNode)),
      bids_(std::move(other.bids_)),
      asks_(std::move(other.asks_)),
      index_(std::move(other.index_)),
      last_accepted_(std::exchange(other.last_accepted_, OrderId{0})) {}

Book& Book::operator=(Book&& other) noexcept {
  if (this != &other) {
    nodes_ = std::move(other.nodes_);
    free_ = std::exchange(other.free_, kNoNode);
    bids_ = std::move(other.bids_);
    asks_ = std::move(other.asks_);
    index_ = std::move(other.index_);
    last_accepted_ = std::exchange(other.last_accepted_, OrderId{0});
    // std::vector's move assignment leaves the source valid but unspecified; a moved-from book is empty.
    other.nodes_.clear();
    other.bids_.clear();
    other.asks_.clear();
  }
  return *this;
}

void Book::apply(const Command& command, std::vector<Event>& events) {
  if (const auto* order = std::get_if<NewOrder>(&command)) {
    new_order(*order, events);
  } else {
    cancel(std::get<CancelOrder>(command), events);
  }
}

// DESIGN.md, "Validation": the checks in their specified order, so the first rule broken is the reason.
std::optional<RejectReason> Book::validate(const NewOrder& order) const {
  if (order.id <= last_accepted_) return RejectReason::IdNotIncreasing;
  if (!valid_quantity(order.quantity)) return RejectReason::InvalidQuantity;
  const bool price_ok = order.type == OrderType::Market ? order.price == Price{0} : valid_price(order.price);
  if (!price_ok) return RejectReason::InvalidPrice;
  return std::nullopt;
}

void Book::new_order(const NewOrder& order, std::vector<Event>& events) {
  if (const auto reason = validate(order)) {
    events.push_back(Rejected{order.id, *reason});
    return;
  }
  last_accepted_ = order.id;
  events.push_back(Accepted{order.id});

  // Rule 2: the best maker is at the head of the back level, since each side keeps its levels worst first and each
  // queue keeps arrival order. Each pass of the outer loop empties a level, or stops inside it with the taker filled.
  Quantity remaining = order.quantity;
  Ladder& makers = ladder(opposite(order.side));
  while (remaining > Quantity{0} && !makers.empty() && crosses(order, makers.back().price)) {
    PriceLevel& level = makers.back();
    while (remaining > Quantity{0} && level.head != kNoNode) {
      OrderNode& maker = nodes_[level.head];
      // Rule 3: at the maker's price, for the smaller remaining quantity.
      const Quantity fill = std::min(remaining, maker.remaining);
      events.push_back(Trade{order.id, maker.id, fill, level.price});
      remaining -= fill;
      maker.remaining -= fill;
      level.total -= fill;
      // Rule 4: a filled maker leaves; a partly filled one keeps its place at the front.
      if (maker.remaining == Quantity{0}) {
        index_.take(maker.id.value);
        const NodeIndex filled = level.head;
        level.head = maker.next;
        if (level.head == kNoNode) {
          level.tail = kNoNode;
        } else {
          nodes_[level.head].prev = kNoNode;
        }
        release(filled);
      }
    }
    if (level.head == kNoNode) makers.pop_back();
  }

  // Rule 5: a limit order's remainder rests behind everything already at its price; an IOC's or market order's is
  // cancelled.
  if (remaining == Quantity{0}) return;
  if (order.type == OrderType::Limit) {
    rest(order, remaining);
  } else {
    events.push_back(Cancelled{order.id, remaining, CancelReason::Unfilled});
  }
}

Book::Ladder::iterator Book::find_level(Ladder& levels, Side side, Price price) {
  // Bids are in rising price and asks in falling price, so "worse than `price`" is below it for bids, above for asks.
  if (side == Side::Buy) {
    return std::lower_bound(levels.begin(), levels.end(), price,
                            [](const PriceLevel& level, Price p) { return level.price < p; });
  }
  return std::lower_bound(levels.begin(), levels.end(), price,
                          [](const PriceLevel& level, Price p) { return level.price > p; });
}

void Book::rest(const NewOrder& order, Quantity remaining) {
  // The level is made if this is the first order at its price. A taker never rests on a price it could have traded
  // at, so the order can't cross the other side here.
  Ladder& levels = ladder(order.side);
  auto level = find_level(levels, order.side, order.price);
  if (level == levels.end() || level->price != order.price) {
    level = levels.insert(level, PriceLevel{order.price, Quantity{0}, kNoNode, kNoNode});
  }
  const NodeIndex node = allocate();  // may grow the pool, so no reference into it is held across this
  nodes_[node] = OrderNode{order.id, remaining, order.price, level->tail, kNoNode, order.side};
  if (level->tail == kNoNode) {
    level->head = node;
  } else {
    nodes_[level->tail].next = node;
  }
  level->tail = node;
  level->total += remaining;
  // Ids increase, so the id isn't in the index already.
  index_.insert(order.id.value, node);
}

void Book::cancel(const CancelOrder& cancel, std::vector<Event>& events) {
  const NodeIndex node = index_.take(cancel.id.value);
  if (node == kNoNode) {
    events.push_back(Rejected{cancel.id, RejectReason::UnknownOrder});
    return;
  }
  const OrderNode& order = nodes_[node];
  events.push_back(Cancelled{cancel.id, order.remaining, CancelReason::Requested});
  Ladder& levels = ladder(order.side);
  const auto level = find_level(levels, order.side, order.price);  // there: the order rests on it
  level->total -= order.remaining;
  if (order.prev == kNoNode) {
    level->head = order.next;
  } else {
    nodes_[order.prev].next = order.next;
  }
  if (order.next == kNoNode) {
    level->tail = order.prev;
  } else {
    nodes_[order.next].prev = order.prev;
  }
  release(node);  // after the last use of `order`
  if (level->head == kNoNode) levels.erase(level);
}

NodeIndex Book::allocate() {
  if (free_ != kNoNode) {
    const NodeIndex node = free_;
    free_ = nodes_[node].next;
    return node;
  }
  // kNoNode itself is never a node's index.
  if (nodes_.size() >= kNoNode) throw std::length_error("orderbook: more resting orders than 32-bit indices can name");
  nodes_.emplace_back();  // allocates only when the pool is past the room made for it
  return static_cast<NodeIndex>(nodes_.size() - 1);
}

void Book::release(NodeIndex node) {
  nodes_[node].next = free_;
  free_ = node;
}

std::optional<Price> Book::best_price(Side side) const {
  const Ladder& levels = ladder(side);
  if (levels.empty()) return std::nullopt;
  return levels.back().price;
}

std::vector<Level> Book::depth(Side side) const {
  const Ladder& levels = ladder(side);
  std::vector<Level> out;
  out.reserve(levels.size());
  for (auto level = levels.rbegin(); level != levels.rend(); ++level) out.push_back(Level{level->price, level->total});
  return out;
}

std::vector<RestingOrder> Book::orders(Side side) const {
  const Ladder& levels = ladder(side);
  std::vector<RestingOrder> resting;
  for (auto level = levels.rbegin(); level != levels.rend(); ++level) {
    for (NodeIndex node = level->head; node != kNoNode; node = nodes_[node].next) {
      resting.push_back(RestingOrder{nodes_[node].id, side, level->price, nodes_[node].remaining});
    }
  }
  return resting;
}

}  // namespace orderbook
