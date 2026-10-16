#include "orderbook/check.hpp"

#include <algorithm>
#include <variant>

namespace orderbook {
namespace {

bool crosses(const NewOrder& taker, Price maker_price) {
  if (taker.type == OrderType::Market) return true;
  return taker.side == Side::Buy ? maker_price <= taker.price : maker_price >= taker.price;
}

std::string describe(const std::vector<Event>& events) {
  std::string text;
  for (const Event& event : events) {
    if (!text.empty()) text += "; ";
    text += format(event);
  }
  return text.empty() ? "(nothing)" : text;
}

}  // namespace

std::string describe(const std::vector<RestingOrder>& orders) {
  std::string text;
  for (const RestingOrder& order : orders) {
    if (!text.empty()) text += ' ';
    text += std::to_string(order.id.value) + ":" + std::to_string(order.price.ticks) + "x" +
            std::to_string(order.remaining.lots);
  }
  return text;
}

void Audit::problem(const std::string& text) {
  if (problems_.size() < 20) problems_.push_back("command " + std::to_string(commands_) + " '" + current_ + "': " + text);
}

void Audit::record(const Command& command, const std::vector<Event>& events) {
  ++commands_;
  current_ = format(command);
  if (const auto* order = std::get_if<NewOrder>(&command)) {
    record_new(*order, events);
  } else {
    record_cancel(std::get<CancelOrder>(command), events);
  }
}

std::optional<OrderId> Audit::best(Side side) const {
  std::optional<OrderId> best_id;
  const Live* best_order = nullptr;
  for (const auto& [id, order] : live_) {
    if (order.side != side) continue;
    if (best_order == nullptr || ahead_by_price(side, order.price, best_order->price) ||
        (order.price == best_order->price && order.arrival < best_order->arrival)) {
      best_id = OrderId{id};
      best_order = &order;
    }
  }
  return best_id;
}

std::vector<RestingOrder> Audit::expected(Side side) const {
  std::vector<std::pair<std::uint64_t, RestingOrder>> orders;  // arrival, order
  for (const auto& [id, order] : live_) {
    if (order.side == side) orders.push_back({order.arrival, RestingOrder{OrderId{id}, side, order.price, order.remaining}});
  }
  std::sort(orders.begin(), orders.end(), [side](const auto& a, const auto& b) {
    if (a.second.price != b.second.price) return ahead_by_price(side, a.second.price, b.second.price);
    return a.first < b.first;
  });
  std::vector<RestingOrder> sorted;
  for (const auto& entry : orders) sorted.push_back(entry.second);
  return sorted;
}

void Audit::record_new(const NewOrder& order, const std::vector<Event>& events) {
  // Validation, in its specified order.
  std::optional<RejectReason> reason;
  if (order.id <= last_accepted_) {
    reason = RejectReason::IdNotIncreasing;
  } else if (!valid_quantity(order.quantity)) {
    reason = RejectReason::InvalidQuantity;
  } else if (order.type == OrderType::Market ? order.price != Price{0} : !valid_price(order.price)) {
    reason = RejectReason::InvalidPrice;
  }
  if (reason) {
    const std::vector<Event> wanted{Rejected{order.id, *reason}};
    if (events != wanted) problem("expected '" + describe(wanted) + "', got '" + describe(events) + "'");
    return;
  }
  if (events.empty() || events.front() != Event{Accepted{order.id}}) {
    problem("expected 'accepted " + std::to_string(order.id.value) + "' first, got '" + describe(events) + "'");
    return;
  }
  last_accepted_ = order.id;

  Quantity remaining = order.quantity;
  std::size_t next = 1;
  for (; next < events.size() && std::holds_alternative<Trade>(events[next]); ++next) {
    const Trade& trade = std::get<Trade>(events[next]);
    const std::string what = "'" + format(trade) + "'";
    if (trade.taker != order.id) return problem(what + ": the taker isn't the new order");
    const std::optional<OrderId> maker_id = best(opposite(order.side));
    if (!maker_id) return problem(what + ": nothing rests on the other side");
    if (trade.maker != *maker_id) {  // I2
      return problem(what + ": the best resting order was " + std::to_string(maker_id->value));
    }
    Live& maker = live_.at(maker_id->value);
    if (trade.price != maker.price) {  // I3
      return problem(what + ": the maker's price is " + std::to_string(maker.price.ticks));
    }
    if (!crosses(order, maker.price)) return problem(what + ": past the taker's limit");  // I3
    const Quantity fill = std::min(remaining, maker.remaining);
    if (trade.quantity != fill) {  // I4
      return problem(what + ": the smaller remaining quantity is " + std::to_string(fill.lots));
    }
    remaining -= fill;
    maker.remaining -= fill;
    if (maker.remaining == Quantity{0}) live_.erase(maker_id->value);
  }

  // Matching rule 2: the taker stops only when it is filled or no longer crosses the best resting order.
  if (remaining > Quantity{0}) {
    if (const std::optional<OrderId> maker_id = best(opposite(order.side))) {
      if (crosses(order, live_.at(maker_id->value).price)) {
        return problem("stopped with " + std::to_string(remaining.lots) + " lots left, still crossing order " +
                       std::to_string(maker_id->value));
      }
    }
  }

  // Rule 5: a limit order's remainder rests silently; an IOC's or market order's is cancelled.
  const std::vector<Event> rest(events.begin() + static_cast<std::ptrdiff_t>(next), events.end());
  std::vector<Event> wanted_rest;
  if (order.type != OrderType::Limit && remaining > Quantity{0}) {
    wanted_rest.push_back(Cancelled{order.id, remaining, CancelReason::Unfilled});
  }
  if (rest != wanted_rest) {
    return problem("after its trades expected '" + describe(wanted_rest) + "', got '" + describe(rest) + "'");
  }
  if (order.type == OrderType::Limit && remaining > Quantity{0}) {
    live_[order.id.value] = Live{order.side, order.price, remaining, arrivals_++};
  }
}

void Audit::record_cancel(const CancelOrder& cancel, const std::vector<Event>& events) {
  std::vector<Event> wanted;
  const auto it = live_.find(cancel.id.value);
  if (it == live_.end()) {
    wanted.push_back(Rejected{cancel.id, RejectReason::UnknownOrder});
  } else {
    wanted.push_back(Cancelled{cancel.id, it->second.remaining, CancelReason::Requested});
    live_.erase(it);
  }
  if (events != wanted) problem("expected '" + describe(wanted) + "', got '" + describe(events) + "'");
}

}  // namespace orderbook
