#include "orderbook/differential.hpp"

namespace orderbook {

std::string format_events(const std::vector<Event>& events) {
  if (events.empty()) return "nothing";
  std::string text;
  for (const Event& event : events) {
    if (!text.empty()) text += " | ";
    text += format(event);
  }
  return text;
}

std::string format_price(const std::optional<Price>& price) {
  return price ? std::to_string(price->ticks) : "none";
}

}  // namespace orderbook
