#include "orderbook/text.hpp"

#include <charconv>
#include <cstdint>
#include <ostream>
#include <system_error>
#include <utility>

namespace orderbook {
namespace {

template <class... Fs>
struct Overloaded : Fs... {
  using Fs::operator()...;
};
template <class... Fs>
Overloaded(Fs...) -> Overloaded<Fs...>;

std::vector<std::string_view> split_words(std::string_view line) {
  std::vector<std::string_view> words;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    const std::size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
    if (i > start) words.push_back(line.substr(start, i - start));
  }
  return words;
}

// A whole word as a decimal integer: no sign for unsigned types, an optional '-' for signed ones, no '+', no
// spaces, nothing after the digits, and in range.
template <typename Int>
std::optional<Int> parse_int(std::string_view word) {
  Int value{};
  const char* const last = word.data() + word.size();
  const auto [ptr, ec] = std::from_chars(word.data(), last, value);
  if (word.empty() || ec != std::errc{} || ptr != last) return std::nullopt;
  return value;
}

template <typename T>
std::optional<T> fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return std::nullopt;
}

std::string quoted(std::string_view word) { return "'" + std::string(word) + "'"; }

std::optional<Command> parse_new(const std::vector<std::string_view>& words, std::string* error) {
  constexpr std::string_view usage = "expected: new <id> <buy|sell> <limit|ioc|market> <quantity> [@ <price>]";
  if (words.size() != 5 && words.size() != 7) return fail<Command>(error, std::string(usage));
  const auto id = parse_int<std::uint64_t>(words[1]);
  if (!id) return fail<Command>(error, "bad order id " + quoted(words[1]));
  const auto side = parse_side(words[2]);
  if (!side) return fail<Command>(error, "bad side " + quoted(words[2]) + ", expected buy or sell");
  const auto type = parse_order_type(words[3]);
  if (!type) return fail<Command>(error, "bad order type " + quoted(words[3]) + ", expected limit, ioc or market");
  const auto quantity = parse_int<std::int64_t>(words[4]);
  if (!quantity) return fail<Command>(error, "bad quantity " + quoted(words[4]));

  std::int64_t price = 0;
  if (words.size() == 7) {
    if (words[5] != "@") return fail<Command>(error, "expected '@' before the price, got " + quoted(words[5]));
    const auto parsed = parse_int<std::int64_t>(words[6]);
    if (!parsed) return fail<Command>(error, "bad price " + quoted(words[6]));
    price = *parsed;
  } else if (*type != OrderType::Market) {
    return fail<Command>(error, std::string(to_string(*type)) + " orders need '@ <price>'");
  }
  return NewOrder{OrderId{*id}, *side, *type, Quantity{*quantity}, Price{price}};
}

}  // namespace

std::string_view to_string(RejectReason reason) {
  switch (reason) {
    case RejectReason::IdNotIncreasing:
      return "id-not-increasing";
    case RejectReason::InvalidQuantity:
      return "invalid-quantity";
    case RejectReason::InvalidPrice:
      return "invalid-price";
    case RejectReason::UnknownOrder:
      return "unknown-order";
  }
  return "?";
}

std::string_view to_string(CancelReason reason) {
  switch (reason) {
    case CancelReason::Requested:
      return "requested";
    case CancelReason::Unfilled:
      return "unfilled";
  }
  return "?";
}

std::string format(const Command& command) {
  return std::visit(
      Overloaded{
          [](const NewOrder& order) {
            std::string line = "new " + std::to_string(order.id.value) + " " + std::string(to_string(order.side)) +
                               " " + std::string(to_string(order.type)) + " " + std::to_string(order.quantity.lots);
            // A market order has no price; one that carries a price anyway keeps it, so the line still parses back
            // to exactly this (invalid) command.
            if (order.type != OrderType::Market || order.price != Price{0}) {
              line += " @ " + std::to_string(order.price.ticks);
            }
            return line;
          },
          [](const CancelOrder& cancel) { return "cancel " + std::to_string(cancel.id.value); },
      },
      command);
}

std::string format(const Event& event) {
  return std::visit(
      Overloaded{
          [](const Accepted& e) { return "accepted " + std::to_string(e.id.value); },
          [](const Rejected& e) {
            return "rejected " + std::to_string(e.id.value) + " " + std::string(to_string(e.reason));
          },
          [](const Trade& e) {
            return "trade " + std::to_string(e.taker.value) + " " + std::to_string(e.maker.value) + " " +
                   std::to_string(e.quantity.lots) + " @ " + std::to_string(e.price.ticks);
          },
          [](const Cancelled& e) {
            return "cancelled " + std::to_string(e.id.value) + " " + std::to_string(e.remaining.lots) + " " +
                   std::string(to_string(e.reason));
          },
      },
      event);
}

std::string format(const std::vector<Level>& levels) {
  std::string text;
  for (const Level& level : levels) {
    if (!text.empty()) text += ' ';
    text += std::to_string(level.price.ticks) + "x" + std::to_string(level.quantity.lots);
  }
  return text;
}

std::string format(const std::vector<RestingOrder>& orders) {
  std::string text;
  for (const RestingOrder& order : orders) {
    if (!text.empty()) text += ' ';
    text += std::to_string(order.id.value) + ":" + std::to_string(order.price.ticks) + "x" +
            std::to_string(order.remaining.lots);
  }
  return text;
}

std::string format_with_sides(const std::vector<RestingOrder>& orders) {
  std::string text;
  for (const RestingOrder& order : orders) {
    if (!text.empty()) text += ' ';
    text += format(std::vector<RestingOrder>{order}) + "(" + std::string(to_string(order.side)) + ")";
  }
  return text;
}

std::optional<Command> parse_command(std::string_view line, std::string* error) {
  const std::vector<std::string_view> words = split_words(line);
  if (words.empty()) return fail<Command>(error, "empty command");
  if (words[0] == "new") return parse_new(words, error);
  if (words[0] == "cancel") {
    if (words.size() != 2) return fail<Command>(error, "expected: cancel <id>");
    const auto id = parse_int<std::uint64_t>(words[1]);
    if (!id) return fail<Command>(error, "bad order id " + quoted(words[1]));
    return CancelOrder{OrderId{*id}};
  }
  return fail<Command>(error, "unknown command " + quoted(words[0]) + ", expected new or cancel");
}

std::optional<std::vector<Level>> parse_levels(std::string_view text, std::string* error) {
  std::vector<Level> levels;
  for (const std::string_view word : split_words(text)) {
    const std::size_t x = word.find('x');
    if (x == std::string_view::npos) {
      return fail<std::vector<Level>>(error, "bad level " + quoted(word) + ", expected <price>x<quantity>");
    }
    const auto price = parse_int<std::int64_t>(word.substr(0, x));
    const auto quantity = parse_int<std::int64_t>(word.substr(x + 1));
    if (!price || !quantity) {
      return fail<std::vector<Level>>(error, "bad level " + quoted(word) + ", expected <price>x<quantity>");
    }
    levels.push_back(Level{Price{*price}, Quantity{*quantity}});
  }
  return levels;
}

std::optional<std::vector<RestingOrder>> parse_orders(std::string_view text, Side side, std::string* error) {
  std::vector<RestingOrder> orders;
  for (const std::string_view word : split_words(text)) {
    const std::size_t colon = word.find(':');
    const std::size_t x = word.find('x', colon == std::string_view::npos ? 0 : colon);
    const auto bad = [&] {
      return fail<std::vector<RestingOrder>>(error,
                                             "bad order " + quoted(word) + ", expected <id>:<price>x<remaining>");
    };
    if (colon == std::string_view::npos || x == std::string_view::npos) return bad();
    const auto id = parse_int<std::uint64_t>(word.substr(0, colon));
    const auto price = parse_int<std::int64_t>(word.substr(colon + 1, x - colon - 1));
    const auto remaining = parse_int<std::int64_t>(word.substr(x + 1));
    if (!id || !price || !remaining) return bad();
    orders.push_back(RestingOrder{OrderId{*id}, side, Price{*price}, Quantity{*remaining}});
  }
  return orders;
}

void PrintTo(const Command& command, std::ostream* out) { *out << format(command); }
void PrintTo(const Event& event, std::ostream* out) { *out << format(event); }
void PrintTo(const Level& level, std::ostream* out) { *out << format(std::vector<Level>{level}); }

}  // namespace orderbook
