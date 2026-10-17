#pragma once

// Scenario files (DESIGN.md, "The text format"): scripts of commands, each followed by the events it must produce
// and, optionally, what a side's depth must then be, and the rest of the book's state. They are written by hand from
// DESIGN.md, or by the differential harness from a shrunk difference, and run against any book with the reference
// model's interface: apply(command, events), depth(side), and for the state checks best_price(side), orders(side),
// size() and last_accepted_id().

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "orderbook/command.hpp"
#include "orderbook/event.hpp"
#include "orderbook/resting_order.hpp"
#include "orderbook/text.hpp"
#include "orderbook/types.hpp"

namespace orderbook {

struct DepthCheck {
  int line = 0;  // where it is in the file, from 1
  Side side = Side::Buy;
  std::vector<Level> levels;  // best first

  bool operator==(const DepthCheck&) const = default;
};

// A check of the book's state beyond depth, so that a file can pin everything the differential harness compares:
//   = best bids 100                    a side's best price, or "none"
//   = orders asks 3:101x5 4:101x2      a side's resting orders in priority order, as id:price x remaining
//   = resting 3                        the number of orders resting on both sides
//   = last-accepted 5                  the last accepted id
struct StateCheck {
  enum class Kind : std::uint8_t { BestPrice, Orders, Resting, LastAccepted };

  int line = 0;  // where it is in the file, from 1
  Kind kind = Kind::Resting;
  Side side = Side::Buy;             // for BestPrice and Orders
  std::optional<Price> best;         // BestPrice
  std::vector<RestingOrder> orders;  // Orders
  std::uint64_t number = 0;          // Resting (a count) and LastAccepted (an id)

  bool operator==(const StateCheck&) const = default;
};

// The check as a line of a scenario file: "= best bids 100", "= orders asks 3:101x5", "= resting 3".
std::string format(const StateCheck& check);

// What `book` shows for the check's kind and side, as a check of the same kind, side and line.
template <typename Book>
StateCheck observe(const Book& book, const StateCheck& check) {
  StateCheck seen{check.line, check.kind, check.side, std::nullopt, {}, 0};
  switch (check.kind) {
    case StateCheck::Kind::BestPrice:
      seen.best = book.best_price(check.side);
      break;
    case StateCheck::Kind::Orders:
      seen.orders = book.orders(check.side);
      break;
    case StateCheck::Kind::Resting:
      seen.number = book.size();
      break;
    case StateCheck::Kind::LastAccepted:
      seen.number = book.last_accepted_id().value;
      break;
  }
  return seen;
}

struct ScenarioStep {
  int line = 0;  // the command's line in the file, from 1
  Command command;
  std::vector<std::string> events;  // the events it must produce, in the text format, in order
  std::vector<DepthCheck> depth;    // checked after its events
  std::vector<StateCheck> state;    // checked after its depth checks

  bool operator==(const ScenarioStep&) const = default;
};

using Scenario = std::vector<ScenarioStep>;

// Parses a scenario file. On failure returns nullopt and, if `error` is given, names the line and what is wrong.
std::optional<Scenario> parse_scenario(std::string_view text, std::string* error = nullptr);

// What run_scenario says when a step doesn't match.
std::string describe_events_mismatch(const ScenarioStep& step, const std::vector<std::string>& actual);
std::string describe_depth_mismatch(const DepthCheck& check, const std::vector<Level>& actual);
std::string describe_state_mismatch(const StateCheck& check, const StateCheck& actual);

// Runs the steps in order against `book` and stops at the first one that doesn't match: returns what was expected
// and what happened, naming the line, or an empty string if every step matched.
template <typename Book>
std::string run_scenario(const Scenario& scenario, Book& book) {
  std::vector<Event> events;
  std::vector<std::string> actual;
  for (const ScenarioStep& step : scenario) {
    events.clear();
    book.apply(step.command, events);
    actual.clear();
    for (const Event& event : events) actual.push_back(format(event));
    if (actual != step.events) return describe_events_mismatch(step, actual);
    for (const DepthCheck& check : step.depth) {
      const std::vector<Level> levels = book.depth(check.side);
      if (levels != check.levels) return describe_depth_mismatch(check, levels);
    }
    for (const StateCheck& check : step.state) {
      const StateCheck seen = observe(book, check);
      if (seen != check) return describe_state_mismatch(check, seen);
    }
  }
  return {};
}

}  // namespace orderbook
