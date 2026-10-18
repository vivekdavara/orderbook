#include "orderbook/map_book.hpp"

#include <algorithm>
#include <iterator>
#include <variant>

namespace orderbook {
namespace {

// Matching rule 1: whether the taker crosses a maker resting at `maker_price`.
bool crosses(const NewOrder& taker, Price maker_price) {
  if (taker.type == OrderType::Market) return true;
  return taker.side == Side::Buy ? maker_price <= taker.price : maker_price >= taker.price;
}

}  // namespace

void MapBook::apply(const Command& command, std::vector<Event>& events) {
  if (const auto* order = std::get_if<NewOrder>(&command)) {
    new_order(*order, events);
  } else {
    cancel(std::get<CancelOrder>(command), events);
  }
}

// DESIGN.md, "Validation": the checks in their specified order, so the first rule broken is the reason.
std::optional<RejectReason> MapBook::validate(const NewOrder& order) const {
  if (order.id <= last_accepted_) return RejectReason::IdNotIncreasing;
  if (!valid_quantity(order.quantity)) return RejectReason::InvalidQuantity;
  const bool price_ok = order.type == OrderType::Market ? order.price == Price{0} : valid_price(order.price);
  if (!price_ok) return RejectReason::InvalidPrice;
  return std::nullopt;
}

void MapBook::new_order(const NewOrder& order, std::vector<Event>& events) {
  if (const auto reason = validate(order)) {
    events.push_back(Rejected{order.id, *reason});
    return;
  }
  last_accepted_ = order.id;
  events.push_back(Accepted{order.id});

  // Rule 2: the best maker is at the front of the first level, since the map keeps levels best first and each queue
  // keeps arrival order. Each pass of the outer loop empties a level, or stops inside it with the taker filled.
  Quantity remaining = order.quantity;
  Ladder& makers = ladder(opposite(order.side));
  while (remaining > Quantity{0} && !makers.empty() && crosses(order, makers.begin()->first)) {
    const auto level = makers.begin();
    const Price price = level->first;
    PriceLevel& at = level->second;
    while (remaining > Quantity{0} && !at.queue.empty()) {
      Order& maker = at.queue.front();
      // Rule 3: at the maker's price, for the smaller remaining quantity.
      const Quantity fill = std::min(remaining, maker.remaining);
      events.push_back(Trade{order.id, maker.id, fill, price});
      remaining -= fill;
      maker.remaining -= fill;
      at.total -= fill;
      // Rule 4: a filled maker leaves; a partly filled one keeps its place at the front.
      if (maker.remaining == Quantity{0}) {
        index_.erase(maker.id.value);
        at.queue.pop_front();
      }
    }
    if (at.queue.empty()) makers.erase(level);
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

void MapBook::rest(const NewOrder& order, Quantity remaining) {
  // The level is made if this is the first order at its price. A taker never rests on a price it could have traded
  // at, so the order can't cross the other side here.
  const auto level = ladder(order.side).try_emplace(order.price).first;
  PriceLevel& at = level->second;
  at.queue.push_back(Order{order.id, remaining});
  at.total += remaining;
  // Ids increase, so the id isn't in the index already.
  index_.emplace(order.id.value, Location{order.side, level, std::prev(at.queue.end())});
}

void MapBook::cancel(const CancelOrder& cancel, std::vector<Event>& events) {
  const auto found = index_.find(cancel.id.value);
  if (found == index_.end()) {
    events.push_back(Rejected{cancel.id, RejectReason::UnknownOrder});
    return;
  }
  const Location where = found->second;
  index_.erase(found);
  PriceLevel& at = where.level->second;
  const Quantity remaining = where.order->remaining;
  events.push_back(Cancelled{cancel.id, remaining, CancelReason::Requested});
  at.total -= remaining;
  at.queue.erase(where.order);
  if (at.queue.empty()) ladder(where.side).erase(where.level);
}

std::optional<Price> MapBook::best_price(Side side) const {
  const Ladder& prices = ladder(side);
  if (prices.empty()) return std::nullopt;
  return prices.begin()->first;
}

std::vector<Level> MapBook::depth(Side side) const {
  std::vector<Level> levels;
  levels.reserve(ladder(side).size());
  for (const auto& [price, at] : ladder(side)) levels.push_back(Level{price, at.total});
  return levels;
}

std::vector<RestingOrder> MapBook::orders(Side side) const {
  std::vector<RestingOrder> resting;
  for (const auto& [price, at] : ladder(side)) {
    for (const Order& order : at.queue) resting.push_back(RestingOrder{order.id, side, price, order.remaining});
  }
  return resting;
}

}  // namespace orderbook
