#include "orderbook/scenario.hpp"

#include <charconv>
#include <system_error>
#include <utility>

namespace orderbook {
namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

std::string_view trim(std::string_view text) {
  while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
  return text;
}

// The words of a line joined by single spaces, the way format() writes events.
std::string normalize(std::string_view line) {
  std::string out;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && is_space(line[i])) ++i;
    const std::size_t start = i;
    while (i < line.size() && !is_space(line[i])) ++i;
    if (i > start) {
      if (!out.empty()) out += ' ';
      out.append(line.substr(start, i - start));
    }
  }
  return out;
}

// A whole word as a decimal integer, as in the text format: digits only for unsigned types, an optional '-' for
// signed ones.
template <typename Int>
std::optional<Int> to_int(std::string_view word) {
  Int value{};
  const char* const last = word.data() + word.size();
  const auto [ptr, ec] = std::from_chars(word.data(), last, value);
  if (word.empty() || ec != std::errc{} || ptr != last) return std::nullopt;
  return value;
}

// The first word of `text` and what follows it, trimmed.
std::pair<std::string_view, std::string_view> first_word(std::string_view text) {
  std::size_t end = 0;
  while (end < text.size() && !is_space(text[end])) ++end;
  return {text.substr(0, end), trim(text.substr(end))};
}

std::optional<Side> side_of(std::string_view word) {
  if (word == "bids") return Side::Buy;
  if (word == "asks") return Side::Sell;
  return std::nullopt;
}

// A state check's line after '=' (StateCheck), or nullopt with the reason in `error`.
std::optional<StateCheck> parse_state_check(int line, std::string_view text, std::string& error) {
  const auto [what, rest] = first_word(text);
  StateCheck check;
  check.line = line;
  if (what == "best" || what == "orders") {
    const auto [side_word, value] = first_word(rest);
    const std::optional<Side> side = side_of(side_word);
    if (!side) {
      error = "expected '= " + std::string(what) + " bids ...' or '= " + std::string(what) + " asks ...'";
      return std::nullopt;
    }
    check.side = *side;
    if (what == "best") {
      check.kind = StateCheck::Kind::BestPrice;
      if (value != "none") {
        const auto price = to_int<std::int64_t>(value);
        if (!price) {
          error = "bad best price '" + std::string(value) + "', expected a price or 'none'";
          return std::nullopt;
        }
        check.best = Price{*price};
      }
      return check;
    }
    check.kind = StateCheck::Kind::Orders;
    auto orders = parse_orders(value, *side, &error);
    if (!orders) return std::nullopt;
    check.orders = std::move(*orders);
    return check;
  }
  if (what == "resting" || what == "last-accepted") {
    check.kind = what == "resting" ? StateCheck::Kind::Resting : StateCheck::Kind::LastAccepted;
    const auto number = to_int<std::uint64_t>(rest);
    if (!number) {
      error = "bad number '" + std::string(rest) + "' for '= " + std::string(what) + "'";
      return std::nullopt;
    }
    check.number = *number;
    return check;
  }
  error = "expected '= bids ...', '= asks ...', '= best ...', '= orders ...', '= resting ...' or "
          "'= last-accepted ...', got '= " + std::string(text) + "'";
  return std::nullopt;
}

std::string list(const std::vector<std::string>& lines) {
  if (lines.empty()) return "  (nothing)\n";
  std::string out;
  for (const std::string& line : lines) out += "  " + line + "\n";
  return out;
}

}  // namespace

std::optional<Scenario> parse_scenario(std::string_view text, std::string* error) {
  Scenario scenario;
  int number = 0;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    std::size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) end = text.size();
    const std::string_view line = trim(text.substr(pos, end - pos));
    pos = end + 1;
    ++number;

    const auto fail = [&](const std::string& message) -> std::optional<Scenario> {
      if (error != nullptr) *error = "line " + std::to_string(number) + ": " + message;
      return std::nullopt;
    };

    if (line.empty() || line.front() == '#') continue;
    if (line.front() == '>') {
      std::string command_error;
      const auto command = parse_command(line.substr(1), &command_error);
      if (!command) return fail(command_error);
      scenario.push_back(ScenarioStep{number, *command, {}, {}, {}});
      continue;
    }
    if (scenario.empty()) return fail("an expectation before any command ('> ...')");
    ScenarioStep& step = scenario.back();

    if (line.front() == '=') {
      const std::string_view rest = trim(line.substr(1));
      const auto [word, after] = first_word(rest);
      if (const std::optional<Side> side = side_of(word)) {
        std::string levels_error;
        auto levels = parse_levels(after, &levels_error);
        if (!levels) return fail(levels_error);
        step.depth.push_back(DepthCheck{number, *side, std::move(*levels)});
        continue;
      }
      std::string state_error;
      auto check = parse_state_check(number, rest, state_error);
      if (!check) return fail(state_error);
      step.state.push_back(std::move(*check));
      continue;
    }
    if (!step.depth.empty() || !step.state.empty()) {
      return fail("an event after the step's depth or state checks; events come first");
    }
    step.events.push_back(normalize(line));
  }
  return scenario;
}

std::string describe_events_mismatch(const ScenarioStep& step, const std::vector<std::string>& actual) {
  return "line " + std::to_string(step.line) + ": '" + format(step.command) + "'\nexpected events:\n" +
         list(step.events) + "actual events:\n" + list(actual);
}

std::string describe_depth_mismatch(const DepthCheck& check, const std::vector<Level>& actual) {
  const auto line = [&](const std::vector<Level>& levels) {
    std::string text = check.side == Side::Buy ? "= bids" : "= asks";
    if (!levels.empty()) text += " " + format(levels);
    return text;
  };
  return "line " + std::to_string(check.line) + ": expected '" + line(check.levels) + "', actual '" + line(actual) +
         "'";
}

std::string format(const StateCheck& check) {
  const std::string side = check.side == Side::Buy ? "bids" : "asks";
  switch (check.kind) {
    case StateCheck::Kind::BestPrice:
      return "= best " + side + " " + (check.best ? std::to_string(check.best->ticks) : "none");
    case StateCheck::Kind::Orders: {
      const std::string orders = format(check.orders);
      return "= orders " + side + (orders.empty() ? "" : " ") + orders;
    }
    case StateCheck::Kind::Resting:
      return "= resting " + std::to_string(check.number);
    case StateCheck::Kind::LastAccepted:
      return "= last-accepted " + std::to_string(check.number);
  }
  return "= ?";
}

std::string describe_state_mismatch(const StateCheck& check, const StateCheck& actual) {
  std::string wanted = format(check);
  std::string got = format(actual);
  // Orders that print the same and still differ, differ in what the line leaves out: each order's side. Show it.
  if (wanted == got && check.kind == StateCheck::Kind::Orders) {
    const std::string orders = check.side == Side::Buy ? "= orders bids " : "= orders asks ";
    wanted = orders + format_with_sides(check.orders);
    got = orders + format_with_sides(actual.orders);
  }
  return "line " + std::to_string(check.line) + ": expected '" + wanted + "', actual '" + got + "'";
}

}  // namespace orderbook
