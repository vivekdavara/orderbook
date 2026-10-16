#pragma once

// Invariant checks (DESIGN.md, "Invariants") for any book with the reference model's queries: orders(side),
// depth(side) and best_price(side). They don't decide anything themselves; they check what a book did.
//
// check_book looks at one moment: I1 (uncrossed) and I5 (a well-formed book). Audit follows a whole run, command
// by command, keeping its own record of every resting order, and checks each command's events against the rules:
// validation and its order, I2 (each trade is with the best order, by price and then arrival), I3 (price limits,
// the maker's price), I4 (conservation: fills are the smaller remaining quantity, cancels report what was left),
// and the order of the events. It finds the best order by scanning its record, so it is O(n) per trade, like the
// reference model, but shares none of its matching code (only the vocabulary types, their bounds and opposite()).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "orderbook/command.hpp"
#include "orderbook/event.hpp"
#include "orderbook/reference_book.hpp"
#include "orderbook/text.hpp"
#include "orderbook/types.hpp"

namespace orderbook {

// Whether price `a` comes before price `b` on `side`: higher for bids, lower for asks.
constexpr bool ahead_by_price(Side side, Price a, Price b) { return side == Side::Buy ? a > b : a < b; }

// "3:101x5 1:102x2": each order as id:price x remaining.
std::string describe(const std::vector<RestingOrder>& orders);

// I1 and I5: each side's orders come in price priority, each has a valid price and 1 to 10^9 lots left, no id
// rests twice, depth is exactly the orders summed by price, the best price is depth's first, and the best bid is
// below the best ask. Returns one line per problem; empty when the book is sound.
template <typename Book>
std::vector<std::string> check_book(const Book& book) {
  std::vector<std::string> problems;
  std::unordered_set<std::uint64_t> ids;
  for (const Side side : {Side::Buy, Side::Sell}) {
    const std::string name = side == Side::Buy ? "bids" : "asks";
    const std::vector<RestingOrder> orders = book.orders(side);
    std::vector<Level> levels;
    for (std::size_t i = 0; i < orders.size(); ++i) {
      const RestingOrder& order = orders[i];
      const std::string what = name + " order " + std::to_string(order.id.value);
      if (order.side != side) problems.push_back(what + " is on the wrong side");
      if (!valid_price(order.price)) problems.push_back(what + " has an invalid price");
      if (!valid_quantity(order.remaining)) problems.push_back(what + " has " + std::to_string(order.remaining.lots) + " lots left");
      if (!ids.insert(order.id.value).second) problems.push_back(what + " rests twice");
      if (i > 0 && ahead_by_price(side, order.price, orders[i - 1].price)) {
        problems.push_back(what + " is out of price priority: " + describe(orders));
      }
      if (levels.empty() || levels.back().price != order.price) levels.push_back(Level{order.price, Quantity{0}});
      levels.back().quantity += order.remaining;
    }
    const std::vector<Level> depth = book.depth(side);
    if (depth != levels) {
      problems.push_back(name + " depth is '" + format(depth) + "' but the orders sum to '" + format(levels) + "'");
    }
    const std::optional<Price> best = book.best_price(side);
    const std::optional<Price> first = levels.empty() ? std::nullopt : std::optional<Price>(levels.front().price);
    if (best != first) problems.push_back(name + " best price isn't the first level's");
  }
  const std::optional<Price> bid = book.best_price(Side::Buy);
  const std::optional<Price> ask = book.best_price(Side::Sell);
  if (bid && ask && !(*bid < *ask)) {
    problems.push_back("crossed: best bid " + std::to_string(bid->ticks) + ", best ask " + std::to_string(ask->ticks));
  }
  return problems;
}

class Audit {
 public:
  // Checks one command's events and updates the record. Call it for every command of a run, in order.
  void record(const Command& command, const std::vector<Event>& events);

  // Checks that a book holds exactly the orders the record says should rest, in priority order (price, then
  // arrival), each with the quantity the events left it.
  template <typename Book>
  void compare(const Book& book) {
    for (const Side side : {Side::Buy, Side::Sell}) {
      const std::vector<RestingOrder> actual = book.orders(side);
      const std::vector<RestingOrder> wanted = expected(side);
      if (actual != wanted) {
        problem(std::string(side == Side::Buy ? "bids" : "asks") + " are '" + describe(actual) + "', expected '" +
                describe(wanted) + "'");
      }
    }
  }

  bool ok() const { return problems_.empty(); }
  // The first problems found (at most 20), each naming the command it was found at.
  const std::vector<std::string>& problems() const { return problems_; }
  // Orders the record says are resting.
  std::size_t resting() const { return live_.size(); }

 private:
  struct Live {
    Side side;
    Price price;
    Quantity remaining;
    std::uint64_t arrival;
  };

  void record_new(const NewOrder& order, const std::vector<Event>& events);
  void record_cancel(const CancelOrder& cancel, const std::vector<Event>& events);
  // The best resting order on a side, by price and then arrival, if there is one.
  std::optional<OrderId> best(Side side) const;
  std::vector<RestingOrder> expected(Side side) const;
  void problem(const std::string& text);

  std::unordered_map<std::uint64_t, Live> live_;
  std::uint64_t arrivals_ = 0;
  OrderId last_accepted_{0};
  std::uint64_t commands_ = 0;
  std::string current_;  // the command being checked, for messages
  std::vector<std::string> problems_;
};

}  // namespace orderbook
