#include "orderbook/reference_book.hpp"

#include <algorithm>
#include <type_traits>
#include <variant>

namespace orderbook {
namespace {

// Whether a resting order at `price` on `side` comes before one at `other` by price: higher for bids, lower for asks.
bool better_price(Side side, Price price, Price other) { return side == Side::Buy ? price > other : price < other; }

// Matching rule 1: whether the taker crosses a maker resting at `maker_price`.
bool crosses(const NewOrder& taker, Price maker_price) {
  if (taker.type == OrderType::Market) return true;
  return taker.side == Side::Buy ? maker_price <= taker.price : maker_price >= taker.price;
}

}  // namespace

void ReferenceBook::apply(const Command& command, std::vector<Event>& events) {
  std::visit(
      [&](const auto& c) {
        if constexpr (std::is_same_v<std::decay_t<decltype(c)>, NewOrder>) {
          new_order(c, events);
        } else {
          cancel(c, events);
        }
      },
      command);
}

// DESIGN.md, "Validation": the checks in their specified order.
std::optional<RejectReason> ReferenceBook::validate(const NewOrder& order) const {
  if (order.id <= last_accepted_) return RejectReason::IdNotIncreasing;
  if (!valid_quantity(order.quantity)) return RejectReason::InvalidQuantity;
  const bool price_ok = order.type == OrderType::Market ? order.price == Price{0} : valid_price(order.price);
  if (!price_ok) return RejectReason::InvalidPrice;
  return std::nullopt;
}

void ReferenceBook::new_order(const NewOrder& order, std::vector<Event>& events) {
  if (const auto reason = validate(order)) {
    events.push_back(Rejected{order.id, *reason});
    return;
  }
  last_accepted_ = order.id;
  events.push_back(Accepted{order.id});

  Quantity remaining = order.quantity;
  std::vector<RestingOrder>& makers = side_orders(opposite(order.side));
  while (remaining > Quantity{0}) {
    // Rule 2, word for word: the best maker is the first order, in arrival order, at the best price. The strict
    // comparison keeps the earlier of two orders at the same price.
    auto best = makers.end();
    for (auto it = makers.begin(); it != makers.end(); ++it) {
      if (best == makers.end() || better_price(it->side, it->price, best->price)) best = it;
    }
    if (best == makers.end() || !crosses(order, best->price)) break;

    // Rule 3: at the maker's price, for the smaller remaining quantity.
    const Quantity fill = std::min(remaining, best->remaining);
    events.push_back(Trade{order.id, best->id, fill, best->price});
    remaining -= fill;
    best->remaining -= fill;
    // Rule 4: a filled maker leaves; a partly filled one stays where it is.
    if (best->remaining == Quantity{0}) makers.erase(best);
  }

  // Rule 5: a limit order's remainder rests behind everything already there; an IOC's or market order's is cancelled.
  if (remaining == Quantity{0}) return;
  if (order.type == OrderType::Limit) {
    side_orders(order.side).push_back(RestingOrder{order.id, order.side, order.price, remaining});
  } else {
    events.push_back(Cancelled{order.id, remaining, CancelReason::Unfilled});
  }
}

void ReferenceBook::cancel(const CancelOrder& cancel, std::vector<Event>& events) {
  for (const Side side : {Side::Buy, Side::Sell}) {
    std::vector<RestingOrder>& resting = side_orders(side);
    const auto it = std::find_if(resting.begin(), resting.end(),
                                 [&](const RestingOrder& order) { return order.id == cancel.id; });
    if (it != resting.end()) {
      events.push_back(Cancelled{it->id, it->remaining, CancelReason::Requested});
      resting.erase(it);
      return;
    }
  }
  events.push_back(Rejected{cancel.id, RejectReason::UnknownOrder});
}

std::optional<Price> ReferenceBook::best_price(Side side) const {
  std::optional<Price> best;
  for (const RestingOrder& order : side_orders(side)) {
    if (!best || better_price(side, order.price, *best)) best = order.price;
  }
  return best;
}

std::vector<RestingOrder> ReferenceBook::orders(Side side) const {
  std::vector<RestingOrder> sorted = side_orders(side);
  // Stable, so orders at the same price stay in arrival order.
  std::stable_sort(sorted.begin(), sorted.end(), [side](const RestingOrder& a, const RestingOrder& b) {
    return better_price(side, a.price, b.price);
  });
  return sorted;
}

std::vector<Level> ReferenceBook::depth(Side side) const {
  std::vector<Level> levels;
  for (const RestingOrder& order : orders(side)) {
    if (levels.empty() || levels.back().price != order.price) {
      levels.push_back(Level{order.price, Quantity{0}});
    }
    levels.back().quantity += order.remaining;
  }
  return levels;
}

}  // namespace orderbook
