#pragma once

// Scenario files (DESIGN.md, "The text format"): scripts of commands, each followed by the events it must produce
// and, optionally, what a side's depth must then be. They are written by hand from DESIGN.md and run against any
// book with the reference model's interface: apply(command, events) and depth(side).

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "orderbook/command.hpp"
#include "orderbook/event.hpp"
#include "orderbook/text.hpp"
#include "orderbook/types.hpp"

namespace orderbook {

struct DepthCheck {
  int line = 0;  // where it is in the file, from 1
  Side side = Side::Buy;
  std::vector<Level> levels;  // best first

  bool operator==(const DepthCheck&) const = default;
};

struct ScenarioStep {
  int line = 0;  // the command's line in the file, from 1
  Command command;
  std::vector<std::string> events;  // the events it must produce, in the text format, in order
  std::vector<DepthCheck> depth;    // checked after its events

  bool operator==(const ScenarioStep&) const = default;
};

using Scenario = std::vector<ScenarioStep>;

// Parses a scenario file. On failure returns nullopt and, if `error` is given, names the line and what is wrong.
std::optional<Scenario> parse_scenario(std::string_view text, std::string* error = nullptr);

// What run_scenario says when a step doesn't match.
std::string describe_events_mismatch(const ScenarioStep& step, const std::vector<std::string>& actual);
std::string describe_depth_mismatch(const DepthCheck& check, const std::vector<Level>& actual);

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
  }
  return {};
}

}  // namespace orderbook
