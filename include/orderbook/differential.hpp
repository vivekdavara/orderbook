#pragma once

// Differential testing (DESIGN.md, "Testing"): the same commands through two books in lockstep, comparing every
// command's events and, as often as asked, the books' state. One book is the expected one (the reference model) and
// the other is under test (the engine).
//
// A disagreement on a long random stream is shrunk with minimize() to the few commands that still show it, and
// written out as a scenario file: each command with the expected book's events, a comment with what the other book
// did differently, and the expected depth at the end. When the books' events differed, the book under test fails
// that file and the expected one passes it, so saved under tests/scenarios/ it becomes a regression test.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "orderbook/check.hpp"
#include "orderbook/command.hpp"
#include "orderbook/event.hpp"
#include "orderbook/minimize.hpp"
#include "orderbook/random_flow.hpp"
#include "orderbook/resting_order.hpp"
#include "orderbook/text.hpp"
#include "orderbook/types.hpp"

namespace orderbook {

// "accepted 3 | trade 3 1 5 @ 101": a command's events on one line; "nothing" for none.
std::string format_events(const std::vector<Event>& events);
// "101" or "none".
std::string format_price(const std::optional<Price>& price);

// The first thing that differs between two books' states: each side's orders (id, price, remaining, in priority
// order), depth and best price, then the number resting and the last accepted id. Nullopt if nothing does.
template <typename Expected, typename Actual>
std::optional<std::string> state_difference(const Expected& expected, const Actual& actual) {
  for (const Side side : {Side::Buy, Side::Sell}) {
    const std::string name = side == Side::Buy ? "bids" : "asks";
    const std::vector<RestingOrder> wanted = expected.orders(side);
    const std::vector<RestingOrder> got = actual.orders(side);
    if (wanted != got) return name + ": expected '" + describe(wanted) + "', actual '" + describe(got) + "'";
    const std::vector<Level> wanted_depth = expected.depth(side);
    const std::vector<Level> got_depth = actual.depth(side);
    if (wanted_depth != got_depth) {
      return name + " depth: expected '" + format(wanted_depth) + "', actual '" + format(got_depth) + "'";
    }
    const std::optional<Price> wanted_best = expected.best_price(side);
    const std::optional<Price> got_best = actual.best_price(side);
    if (wanted_best != got_best) {
      return name + " best price: expected " + format_price(wanted_best) + ", actual " + format_price(got_best);
    }
  }
  if (expected.size() != actual.size()) {
    return "size: expected " + std::to_string(expected.size()) + ", actual " + std::to_string(actual.size());
  }
  if (expected.last_accepted_id() != actual.last_accepted_id()) {
    return "last accepted id: expected " + std::to_string(expected.last_accepted_id().value) + ", actual " +
           std::to_string(actual.last_accepted_id().value);
  }
  return std::nullopt;
}

// Two books fed the same commands.
template <typename Expected, typename Actual>
class Lockstep {
 public:
  // Applies the command to both books. Returns how their events differ, or nullopt if they are the same.
  std::optional<std::string> apply(const Command& command) {
    expected_events_.clear();
    actual_events_.clear();
    expected_.apply(command, expected_events_);
    actual_.apply(command, actual_events_);
    if (expected_events_ == actual_events_) return std::nullopt;
    return "events: expected '" + format_events(expected_events_) + "', actual '" + format_events(actual_events_) +
           "'";
  }

  // How the books' states differ now, or nullopt.
  std::optional<std::string> compare() const { return state_difference(expected_, actual_); }

  const Expected& expected() const { return expected_; }
  const Actual& actual() const { return actual_; }
  // The last command's events from each book.
  const std::vector<Event>& expected_events() const { return expected_events_; }
  const std::vector<Event>& actual_events() const { return actual_events_; }

 private:
  Expected expected_;
  Actual actual_;
  std::vector<Event> expected_events_;
  std::vector<Event> actual_events_;
};

struct Difference {
  std::size_t index = 0;   // the command, from 0, at which (or after which) the books were first seen to differ
  bool in_events = false;  // whether that command's events differed, rather than only the states after it
  std::string what;
};

// Runs the commands through a fresh pair of books, comparing every command's events, and their states after every
// `state_every` commands (never, if 0) and after the last. Returns the first difference seen, or nullopt.
template <typename Expected, typename Actual>
std::optional<Difference> first_difference(const std::vector<Command>& commands, std::size_t state_every = 0) {
  Lockstep<Expected, Actual> books;
  for (std::size_t i = 0; i < commands.size(); ++i) {
    if (auto what = books.apply(commands[i])) {
      return Difference{i, true, "command " + std::to_string(i + 1) + ": " + *what};
    }
    const bool last = i + 1 == commands.size();
    if (last || (state_every > 0 && (i + 1) % state_every == 0)) {
      if (auto what = books.compare()) {
        return Difference{i, false, "after command " + std::to_string(i + 1) + ": " + *what};
      }
    }
  }
  return std::nullopt;
}

// Writes commands as a scenario file of the expected book's events, noting where the other book differs.
template <typename Expected, typename Actual>
std::string differential_scenario(const std::vector<Command>& commands, const std::string& heading) {
  Lockstep<Expected, Actual> books;
  std::string text = heading;
  for (const Command& command : commands) {
    const std::optional<std::string> events = books.apply(command);
    text += "> " + format(command) + "\n";
    for (const Event& event : books.expected_events()) text += format(event) + "\n";
    if (events) {
      text += "# but the book under test gave: " + format_events(books.actual_events()) + "\n";
    } else if (const auto state = books.compare()) {
      text += "# the events agree, but then " + *state + "\n";
    }
  }
  if (commands.empty()) return text;
  for (const Side side : {Side::Buy, Side::Sell}) {
    const std::string levels = format(books.expected().depth(side));
    text += std::string(side == Side::Buy ? "= bids" : "= asks") + (levels.empty() ? "" : " ") + levels + "\n";
  }
  return text;
}

// Shrinks a stream the two books disagree on to a 1-minimal one (every command needed), and writes that as a
// scenario file (differential_scenario). The stream should end where the difference was seen: each replay compares
// events after every command and states only at the end, so it costs one pass. The shrinking keeps the kind of
// difference found first: if some command's events differed, every candidate must show different events, not
// only different states. Otherwise a bug that shows in the state before it shows in an event (a queue in the wrong
// order, before a trade with the wrong maker) shrinks to a stream whose events all agree, and the scenario file
// would pass on the book under test. Returns an empty string if the books agree on `commands`.
template <typename Expected, typename Actual>
std::string shrink_to_scenario(const std::vector<Command>& commands) {
  const std::optional<Difference> found = first_difference<Expected, Actual>(commands);
  if (!found) return {};
  const bool in_events = found->in_events;
  const auto differs = [in_events](const std::vector<Command>& candidate) {
    const std::optional<Difference> difference = first_difference<Expected, Actual>(candidate);
    return difference.has_value() && difference->in_events == in_events;
  };
  const std::vector<Command> shrunk = minimize(commands, differs);
  const std::string heading = "# The books disagree on these " + std::to_string(shrunk.size()) +
                              " commands, shrunk from " + std::to_string(commands.size()) +
                              ".\n# Each command's events are the expected book's.\n";
  return differential_scenario<Expected, Actual>(shrunk, heading);
}

struct StreamDifference {
  std::uint64_t commands = 0;  // commands run: all of them, or up to and including the first difference
  std::optional<Difference> difference;
};

// Runs `count` commands of RandomFlow(seed, options) through both books, comparing every command's events, and the
// states every `state_every` commands (never, if 0) and after the last. Nothing is kept as it runs, so the stream
// can be as long as wanted. What `orderbook diff` runs; on a difference it reports it at once, and then calls
// shrink_random_stream.
template <typename Expected, typename Actual>
StreamDifference diff_random_stream(std::uint64_t seed, const FlowOptions& options, std::uint64_t count,
                                    std::uint64_t state_every) {
  StreamDifference result;
  RandomFlow flow(seed, options);
  Lockstep<Expected, Actual> books;
  for (std::uint64_t i = 1; i <= count; ++i) {
    std::optional<std::string> what = books.apply(flow.next());
    const bool in_events = what.has_value();
    if (!what && (i == count || (state_every > 0 && i % state_every == 0))) what = books.compare();
    if (what) {
      result.commands = i;
      // No cast: uint64_t is size_t on Linux (GCC's -Wuseless-cast), and the same width on macOS.
      result.difference =
          Difference{i - 1, in_events, (in_events ? "command " : "after command ") + std::to_string(i) + ": " + *what};
      return result;
    }
  }
  result.commands = count;
  return result;
}

// Makes the first `commands` commands of RandomFlow(seed, options) again (the same seed gives the same commands) and
// shrinks them to a scenario file (shrink_to_scenario). Separate from diff_random_stream so a caller can report the
// difference before shrinking: the shrinking replays the book under test hundreds of times on parts of the stream,
// and a book with a memory error can crash on one of them.
template <typename Expected, typename Actual>
std::string shrink_random_stream(std::uint64_t seed, const FlowOptions& options, std::uint64_t commands) {
  RandomFlow flow(seed, options);
  std::vector<Command> stream;
  stream.reserve(commands);
  for (std::uint64_t i = 0; i < commands; ++i) stream.push_back(flow.next());
  return shrink_to_scenario<Expected, Actual>(stream);
}

}  // namespace orderbook
