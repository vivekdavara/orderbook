#include "orderbook/scenario.hpp"

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
      scenario.push_back(ScenarioStep{number, *command, {}, {}});
      continue;
    }
    if (scenario.empty()) return fail("an expectation before any command ('> ...')");
    ScenarioStep& step = scenario.back();

    if (line.front() == '=') {
      const std::string_view rest = trim(line.substr(1));
      std::size_t word_end = 0;
      while (word_end < rest.size() && !is_space(rest[word_end])) ++word_end;
      const std::string_view side_word = rest.substr(0, word_end);
      if (side_word != "bids" && side_word != "asks") {
        return fail("expected '= bids ...' or '= asks ...', got '" + std::string(line) + "'");
      }
      std::string levels_error;
      auto levels = parse_levels(rest.substr(word_end), &levels_error);
      if (!levels) return fail(levels_error);
      step.depth.push_back(DepthCheck{number, side_word == "bids" ? Side::Buy : Side::Sell, std::move(*levels)});
      continue;
    }
    if (!step.depth.empty()) return fail("an event after the step's depth checks; events come first");
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

}  // namespace orderbook
