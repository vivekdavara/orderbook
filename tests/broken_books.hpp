#pragma once

// Books with one deliberate bug each, for testing the tools whose job is to catch bugs: the minimizer
// (minimize_test.cpp) and the differential harness (differential_harness_test.cpp). Most are the reference model
// with one thing it reports changed; the last is a book of its own with time priority backwards.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

#include "orderbook/command.hpp"
#include "orderbook/event.hpp"
#include "orderbook/reference_book.hpp"
#include "orderbook/types.hpp"

namespace orderbook::broken {

// The reference model's queries, for the books below to keep or replace.
class WrappedReferenceBook {
 public:
  std::optional<Price> best_price(Side side) const { return book_.best_price(side); }
  std::vector<Level> depth(Side side) const { return book_.depth(side); }
  std::vector<RestingOrder> orders(Side side) const { return book_.orders(side); }
  std::size_t size() const { return book_.size(); }
  OrderId last_accepted_id() const { return book_.last_accepted_id(); }

 protected:
  ReferenceBook book_;
};

// A trade with a maker that has already traded once is reported a tick off its price. Finding it takes three
// commands: a resting order, a taker that fills part of it, and a second taker.
class BookWithAPriceBug : public WrappedReferenceBook {
 public:
  void apply(const Command& command, std::vector<Event>& events) {
    const std::size_t first = events.size();
    book_.apply(command, events);
    for (std::size_t i = first; i < events.size(); ++i) {
      if (auto* trade = std::get_if<Trade>(&events[i]); trade != nullptr && trades_[trade->maker.value]++ > 0) {
        trade->price.ticks += 1;
      }
    }
  }

 private:
  std::unordered_map<std::uint64_t, int> trades_;
};

// Forgets to report a market order's unfilled remainder: one command is enough.
class BookThatDropsAnUnfilledMarketOrder : public WrappedReferenceBook {
 public:
  void apply(const Command& command, std::vector<Event>& events) {
    const std::size_t first = events.size();
    book_.apply(command, events);
    const auto* order = std::get_if<NewOrder>(&command);
    if (order != nullptr && order->type == OrderType::Market && events.size() > first &&
        std::holds_alternative<Cancelled>(events.back())) {
      events.pop_back();
    }
  }
};

// Every event is right; only its depth is wrong, leaving out a side's third level and beyond. Finding it takes
// three orders resting on one side at three prices.
class BookWithShortDepth : public WrappedReferenceBook {
 public:
  void apply(const Command& command, std::vector<Event>& events) { book_.apply(command, events); }

  std::vector<Level> depth(Side side) const {
    std::vector<Level> levels = book_.depth(side);
    if (levels.size() > 2) levels.resize(2);
    return levels;
  }
};

// The three below each get one more of the reported state wrong, with every event right, so that each part of
// state_difference is shown to be compared.

// Reports no best price for a side holding a single order.
class BookWithNoBestPriceForALoneOrder : public WrappedReferenceBook {
 public:
  void apply(const Command& command, std::vector<Event>& events) { book_.apply(command, events); }

  std::optional<Price> best_price(Side side) const {
    if (book_.orders(side).size() == 1) return std::nullopt;
    return book_.best_price(side);
  }
};

// Counts every order a cancel removed as still resting.
class BookThatCountsCancelledOrders : public WrappedReferenceBook {
 public:
  void apply(const Command& command, std::vector<Event>& events) {
    const std::size_t first = events.size();
    book_.apply(command, events);
    for (std::size_t i = first; i < events.size(); ++i) {
      if (const auto* cancelled = std::get_if<Cancelled>(&events[i]);
          cancelled != nullptr && cancelled->reason == CancelReason::Requested) {
        ++cancelled_;
      }
    }
  }

  std::size_t size() const { return book_.size() + cancelled_; }

 private:
  std::size_t cancelled_ = 0;
};

// Reports the highest id of any new order it was sent, accepted or not.
class BookThatReportsTheLastIdSeen : public WrappedReferenceBook {
 public:
  void apply(const Command& command, std::vector<Event>& events) {
    if (const auto* order = std::get_if<NewOrder>(&command); order != nullptr && order->id > seen_) seen_ = order->id;
    book_.apply(command, events);
  }

  OrderId last_accepted_id() const { return seen_; }

 private:
  OrderId seen_{0};
};

// Time priority backwards: at the best price, the newest order trades first, and orders() lists each price's orders
// newest first. Otherwise the reference model's rules, written the same direct way (a vector of each side's orders
// in arrival order). The wrong order shows in the state as soon as two orders rest at one price, but in an event
// only when a taker reaches them: the case where shrinking by state alone would hide the bug from the events.
class BookWithNewestFirstQueues {
 public:
  void apply(const Command& command, std::vector<Event>& events) {
    if (const auto* cancel = std::get_if<CancelOrder>(&command)) {
      for (std::vector<RestingOrder>* resting : {&bids_, &asks_}) {
        const auto it = std::find_if(resting->begin(), resting->end(),
                                     [&](const RestingOrder& order) { return order.id == cancel->id; });
        if (it != resting->end()) {
          events.push_back(Cancelled{it->id, it->remaining, CancelReason::Requested});
          resting->erase(it);
          return;
        }
      }
      events.push_back(Rejected{cancel->id, RejectReason::UnknownOrder});
      return;
    }
    const NewOrder& order = std::get<NewOrder>(command);
    const bool price_ok = order.type == OrderType::Market ? order.price == Price{0} : valid_price(order.price);
    if (order.id <= last_accepted_ || !valid_quantity(order.quantity) || !price_ok) {
      events.push_back(Rejected{order.id, order.id <= last_accepted_   ? RejectReason::IdNotIncreasing
                                          : !valid_quantity(order.quantity) ? RejectReason::InvalidQuantity
                                                                            : RejectReason::InvalidPrice});
      return;
    }
    last_accepted_ = order.id;
    events.push_back(Accepted{order.id});
    Quantity remaining = order.quantity;
    std::vector<RestingOrder>& makers = order.side == Side::Buy ? asks_ : bids_;
    while (remaining > Quantity{0}) {
      auto best = makers.end();
      for (auto it = makers.begin(); it != makers.end(); ++it) {
        // The bug: >= where the rules need >, so a later order at the same price wins.
        if (best == makers.end() || !better(*best, *it)) best = it;
      }
      if (best == makers.end() || !crosses(order, best->price)) break;
      const Quantity fill = std::min(remaining, best->remaining);
      events.push_back(Trade{order.id, best->id, fill, best->price});
      remaining -= fill;
      best->remaining -= fill;
      if (best->remaining == Quantity{0}) makers.erase(best);
    }
    if (remaining == Quantity{0}) return;
    if (order.type == OrderType::Limit) {
      (order.side == Side::Buy ? bids_ : asks_).push_back(RestingOrder{order.id, order.side, order.price, remaining});
    } else {
      events.push_back(Cancelled{order.id, remaining, CancelReason::Unfilled});
    }
  }

  std::optional<Price> best_price(Side side) const {
    const std::vector<RestingOrder> sorted = orders(side);
    return sorted.empty() ? std::nullopt : std::optional<Price>(sorted.front().price);
  }
  std::vector<Level> depth(Side side) const {
    std::vector<Level> levels;
    for (const RestingOrder& order : orders(side)) {
      if (levels.empty() || levels.back().price != order.price) levels.push_back(Level{order.price, Quantity{0}});
      levels.back().quantity += order.remaining;
    }
    return levels;
  }
  // Newest first within a price, as this book trades them.
  std::vector<RestingOrder> orders(Side side) const {
    std::vector<RestingOrder> sorted(side == Side::Buy ? bids_ : asks_);
    std::reverse(sorted.begin(), sorted.end());
    std::stable_sort(sorted.begin(), sorted.end(), [](const RestingOrder& a, const RestingOrder& b) {
      return a.side == Side::Buy ? a.price > b.price : a.price < b.price;
    });
    return sorted;
  }
  std::size_t size() const { return bids_.size() + asks_.size(); }
  OrderId last_accepted_id() const { return last_accepted_; }

 private:
  // Whether `a` beats `b` by price alone (higher bids, lower asks).
  static bool better(const RestingOrder& a, const RestingOrder& b) {
    return a.side == Side::Buy ? a.price > b.price : a.price < b.price;
  }
  static bool crosses(const NewOrder& taker, Price maker_price) {
    if (taker.type == OrderType::Market) return true;
    return taker.side == Side::Buy ? maker_price <= taker.price : maker_price >= taker.price;
  }

  std::vector<RestingOrder> bids_;
  std::vector<RestingOrder> asks_;
  OrderId last_accepted_{0};
};

}  // namespace orderbook::broken
