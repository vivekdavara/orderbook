#pragma once

// Books with one deliberate bug each, for testing the tools whose job is to catch bugs: the minimizer
// (minimize_test.cpp) and the differential harness (differential_test.cpp). Each is the reference model with one
// thing it reports changed.

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

}  // namespace orderbook::broken
