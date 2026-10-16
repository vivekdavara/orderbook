#pragma once

// The one-line text form of commands, events and depth (DESIGN.md, "The text format"), used by scenario files, the
// CLI and failing tests. Every command the types can hold, invalid ones included, formats to a line that parses
// back to the same command.

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "orderbook/command.hpp"
#include "orderbook/event.hpp"
#include "orderbook/types.hpp"

namespace orderbook {

// One price level as depth reports it: a price and the total quantity resting there.
struct Level {
  Price price;
  Quantity quantity;

  bool operator==(const Level&) const = default;
};

std::string_view to_string(RejectReason reason);
std::string_view to_string(CancelReason reason);

// "new 1 buy limit 10 @ 101", "new 2 sell market 5", "cancel 1".
std::string format(const Command& command);
// "accepted 1", "rejected 1 invalid-price", "trade 3 1 10 @ 101", "cancelled 3 5 unfilled".
std::string format(const Event& event);
// "101x10 100x5": levels best first, separated by spaces; empty for an empty side.
std::string format(const std::vector<Level>& levels);

// Parses one command line. Words are separated by spaces or tabs. On failure returns nullopt and, if `error` is
// given, says what was wrong.
std::optional<Command> parse_command(std::string_view line, std::string* error = nullptr);
// Parses the levels of a depth line, the format() of a vector of levels.
std::optional<std::vector<Level>> parse_levels(std::string_view text, std::string* error = nullptr);

// Let GoogleTest print commands, events and levels in their text form when an assertion fails.
void PrintTo(const Command& command, std::ostream* out);
void PrintTo(const Event& event, std::ostream* out);
void PrintTo(const Level& level, std::ostream* out);

}  // namespace orderbook
