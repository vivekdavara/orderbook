#include "orderbook/market_data.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <variant>

namespace orderbook {
namespace {

std::string side_text(const std::vector<Level>& levels) { return levels.empty() ? "-" : format(levels); }

[[noreturn]] void disagree(const std::string& what) {
  throw std::logic_error("the depth feed disagrees with the book's events: " + what);
}

// Notes that the level at (side, price) changed, once per command. The levels one command changes are distinct
// consecutive runs: a taker's trades walk the other side best first and never come back to a level, and its
// remainder rests on its own side; a cancel changes one level. So comparing with the last entry is enough.
void touch(std::vector<LevelUpdate>& updates, std::size_t first, Side side, Price price) {
  if (updates.size() == first || updates.back().side != side || updates.back().price != price) {
    updates.push_back(LevelUpdate{side, price, Quantity{0}});
  }
}

}  // namespace

std::string format(const LevelUpdate& update) {
  return "level " + std::string(to_string(update.side)) + " " + std::to_string(update.price.ticks) + "x" +
         std::to_string(update.total.lots);
}

std::string format(const L2Snapshot& snapshot) {
  return "snapshot " + std::to_string(snapshot.sequence) + " bids " + side_text(snapshot.bids) + " asks " +
         side_text(snapshot.asks);
}

void LevelTotals::add(Price price, Quantity change) {
  const auto it = levels_.try_emplace(price, Quantity{0}).first;
  it->second += change;
  if (it->second.lots == 0) levels_.erase(it);
}

void LevelTotals::set(Price price, Quantity total) {
  if (total.lots == 0) {
    levels_.erase(price);
  } else {
    levels_.insert_or_assign(price, total);
  }
}

Quantity LevelTotals::total(Price price) const {
  const auto it = levels_.find(price);
  return it == levels_.end() ? Quantity{0} : it->second;
}

std::vector<Level> LevelTotals::top(std::size_t levels) const {
  std::vector<Level> out;
  out.reserve(std::min(levels, levels_.size()));
  for (const auto& [price, total] : levels_) {
    if (out.size() == levels) break;
    out.push_back(Level{price, total});
  }
  return out;
}

std::uint64_t DepthFeed::apply(const Command& command, std::span<const Event> events,
                               std::vector<LevelUpdate>& updates) {
  const std::size_t first = updates.size();
  if (const auto* order = std::get_if<NewOrder>(&command)) {
    follow_new_order(*order, events, updates, first);
  } else {
    follow_cancel(std::get<CancelOrder>(command), events, updates, first);
  }
  for (std::size_t i = first; i < updates.size(); ++i) {
    updates[i].total = ladder(updates[i].side).total(updates[i].price);
  }
  return ++sequence_;
}

void DepthFeed::follow_new_order(const NewOrder& order, std::span<const Event> events,
                                 std::vector<LevelUpdate>& updates, std::size_t first) {
  if (events.empty()) disagree("no events for new order " + std::to_string(order.id.value));
  if (std::holds_alternative<Rejected>(events.front())) return;  // a rejection changes nothing
  Quantity remaining = order.quantity;
  bool cancelled = false;
  for (const Event& event : events.subspan(1)) {
    if (const auto* trade = std::get_if<Trade>(&event)) {
      if (trade->taker != order.id || trade->quantity > remaining) {
        disagree("trade " + format(event) + " for new order " + std::to_string(order.id.value));
      }
      take(trade->maker, opposite(order.side), trade->price, trade->quantity, updates, first);
      remaining -= trade->quantity;
    } else if (std::holds_alternative<Cancelled>(event)) {
      cancelled = true;  // what an IOC or market order couldn't trade: it never rests
    }
  }
  if (order.type == OrderType::Limit && remaining.lots > 0 && !cancelled) {
    orders_.emplace(order.id.value, Order{order.side, order.price, remaining});
    ladder(order.side).add(order.price, remaining);
    touch(updates, first, order.side, order.price);
  }
}

void DepthFeed::follow_cancel(const CancelOrder& cancel, std::span<const Event> events,
                              std::vector<LevelUpdate>& updates, std::size_t first) {
  for (const Event& event : events) {
    const auto* cancelled = std::get_if<Cancelled>(&event);
    if (cancelled == nullptr) continue;  // a rejection: no such resting order
    const auto it = orders_.find(cancel.id.value);
    if (cancelled->id != cancel.id || it == orders_.end() || it->second.remaining != cancelled->remaining) {
      disagree(format(event) + " for cancel " + std::to_string(cancel.id.value));
    }
    take(cancel.id, it->second.side, it->second.price, cancelled->remaining, updates, first);
  }
}

void DepthFeed::take(OrderId id, Side side, Price price, Quantity quantity, std::vector<LevelUpdate>& updates,
                     std::size_t first) {
  const auto it = orders_.find(id.value);
  if (it == orders_.end() || it->second.side != side || it->second.price != price ||
      it->second.remaining < quantity) {
    disagree("order " + std::to_string(id.value) + " isn't resting with " + std::to_string(quantity.lots) +
             " lots at " + std::to_string(price.ticks) + " on the " + std::string(to_string(side)) + " side");
  }
  it->second.remaining -= quantity;
  if (it->second.remaining.lots == 0) orders_.erase(it);
  ladder(side).add(price, Quantity{0} - quantity);
  touch(updates, first, side, price);
}

L2Snapshot DepthFeed::snapshot(std::size_t levels) const {
  return L2Snapshot{sequence_, bids_.top(levels), asks_.top(levels)};
}

L2Book::L2Book(const L2Snapshot& snapshot) : sequence_(snapshot.sequence) {
  for (const Level& level : snapshot.bids) bids_.set(level.price, level.quantity);
  for (const Level& level : snapshot.asks) asks_.set(level.price, level.quantity);
}

bool L2Book::apply(std::uint64_t sequence, std::span<const LevelUpdate> updates) {
  if (sequence != sequence_ + 1) return false;
  for (const LevelUpdate& update : updates) {
    (update.side == Side::Buy ? bids_ : asks_).set(update.price, update.total);
  }
  sequence_ = sequence;
  return true;
}

L2Snapshot L2Book::snapshot(std::size_t levels) const {
  return L2Snapshot{sequence_, bids_.top(levels), asks_.top(levels)};
}

}  // namespace orderbook
