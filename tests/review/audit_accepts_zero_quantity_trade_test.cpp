// Review finding: the Audit accepts a 0-lot trade after the taker is filled.
//
// DESIGN.md, Matching rule 2: the taker trades "while the taker has quantity left"; the Events table: "one trade per
// maker it traded with". check.cpp's trade loop (src/check.cpp:100-121) never requires the taker to have quantity
// left before a trade: once `remaining` is 0, the expected fill min(remaining, maker.remaining) is 0, so a trade of
// 0 lots with the current best maker passes every check, and so does any number of them.

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "orderbook/check.hpp"
#include "orderbook/reference_book.hpp"
#include "orderbook/text.hpp"

namespace orderbook {
namespace {

std::string joined(const std::vector<std::string>& lines) {
  std::string text;
  for (const std::string& line : lines) text += line + "\n";
  return text;
}

// An audit that has followed the reference model through `lines`.
Audit audit_after(const std::vector<std::string_view>& lines) {
  ReferenceBook book;
  Audit audit;
  std::vector<Event> events;
  for (const std::string_view line : lines) {
    const Command command = *parse_command(line);
    events.clear();
    book.apply(command, events);
    audit.record(command, events);
  }
  EXPECT_TRUE(audit.ok()) << joined(audit.problems());
  return audit;
}

TEST(ReviewAudit, RejectsAZeroLotTradeAfterALimitTakerIsFilled) {
  // Asks: 1 (5 @ 101), then 2 (5 @ 101). The right answer to "new 3 buy limit 5 @ 101" is "accepted 3",
  // "trade 3 1 5 @ 101", and nothing else.
  Audit audit = audit_after({"new 1 sell limit 5 @ 101", "new 2 sell limit 5 @ 101"});
  audit.record(*parse_command("new 3 buy limit 5 @ 101"),
               {Accepted{OrderId{3}}, Trade{OrderId{3}, OrderId{1}, Quantity{5}, Price{101}},
                Trade{OrderId{3}, OrderId{2}, Quantity{0}, Price{101}}});
  EXPECT_FALSE(audit.ok()) << "a 0-lot trade with order 2 after the taker was filled was accepted";
}

TEST(ReviewAudit, RejectsZeroLotTradesAfterAMarketTakerIsFilled) {
  Audit audit = audit_after({"new 1 sell limit 5 @ 101", "new 2 sell limit 5 @ 102"});
  audit.record(*parse_command("new 3 buy market 5"),
               {Accepted{OrderId{3}}, Trade{OrderId{3}, OrderId{1}, Quantity{5}, Price{101}},
                Trade{OrderId{3}, OrderId{2}, Quantity{0}, Price{102}},
                Trade{OrderId{3}, OrderId{2}, Quantity{0}, Price{102}}});
  EXPECT_FALSE(audit.ok()) << "two 0-lot trades after the market order was filled were accepted";
}

}  // namespace
}  // namespace orderbook
