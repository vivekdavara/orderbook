#include "orderbook/minimize.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "orderbook/check.hpp"
#include "orderbook/random_flow.hpp"
#include "orderbook/reference_book.hpp"
#include "orderbook/text.hpp"

namespace orderbook {
namespace {

std::vector<std::string> lines(const std::vector<Command>& commands) {
  std::vector<std::string> out;
  for (const Command& command : commands) out.push_back(format(command));
  return out;
}

std::vector<Command> cancels(std::uint64_t count) {
  std::vector<Command> commands;
  for (std::uint64_t id = 1; id <= count; ++id) commands.push_back(CancelOrder{OrderId{id}});
  return commands;
}

bool contains(const std::vector<Command>& commands, std::uint64_t id) {
  return std::find(commands.begin(), commands.end(), Command{CancelOrder{OrderId{id}}}) != commands.end();
}

TEST(Minimize, KeepsExactlyTheCommandsTheFailureNeeds) {
  int calls = 0;
  const auto fails = [&](const std::vector<Command>& commands) {
    ++calls;
    return contains(commands, 37) && contains(commands, 81);
  };
  EXPECT_EQ(lines(minimize(cancels(100), fails)), (std::vector<std::string>{"cancel 37", "cancel 81"}));
  // ddmin's bound when chunks fall away easily is O(n log n); 100 commands took this many replays.
  EXPECT_LT(calls, 200);
}

TEST(Minimize, ReturnsAStreamThatDoesNotFailUnchanged) {
  int calls = 0;
  const auto never = [&](const std::vector<Command>&) {
    ++calls;
    return false;
  };
  EXPECT_EQ(minimize(cancels(10), never), cancels(10));
  EXPECT_EQ(calls, 1);
}

TEST(Minimize, CanRemoveEverything) {
  const auto always = [](const std::vector<Command>&) { return true; };
  EXPECT_TRUE(minimize(cancels(10), always).empty());
  EXPECT_TRUE(minimize(cancels(1), always).empty());
}

TEST(Minimize, KeepsOrder) {
  const auto fails = [](const std::vector<Command>& commands) {
    // Fails only if 5 comes before 3, which the original order has.
    const auto five = std::find(commands.begin(), commands.end(), Command{CancelOrder{OrderId{5}}});
    const auto three = std::find(commands.begin(), commands.end(), Command{CancelOrder{OrderId{3}}});
    return five != commands.end() && three != commands.end() && five < three;
  };
  std::vector<Command> commands = cancels(8);
  std::swap(commands[2], commands[4]);  // ... 5, 4, 3 ...
  EXPECT_EQ(lines(minimize(commands, fails)), (std::vector<std::string>{"cancel 5", "cancel 3"}));
}

// The reference model with one deliberate bug, applied to its events: a trade with a maker that has already traded
// once is reported a tick off its price. Finding it takes three commands: a resting order, a taker that fills part of
// it, and a second taker.
class BookWithAPriceBug {
 public:
  void apply(const Command& command, std::vector<Event>& events) {
    const std::size_t first = events.size();
    book_.apply(command, events);
    for (std::size_t i = first; i < events.size(); ++i) {
      if (auto* trade = std::get_if<Trade>(&events[i]); trade != nullptr && trades_[trade->maker.value]++ > 0) {
        trade->price.ticks += 1;
      }
    }
  }

 private:
  ReferenceBook book_;
  std::unordered_map<std::uint64_t, int> trades_;
};

// And one that forgets to report a market order's unfilled remainder: one command is enough.
class BookThatDropsAnUnfilledMarketOrder {
 public:
  void apply(const Command& command, std::vector<Event>& events) {
    const std::size_t first = events.size();
    book_.apply(command, events);
    const auto* order = std::get_if<NewOrder>(&command);
    if (order != nullptr && order->type == OrderType::Market && events.size() > first &&
        std::holds_alternative<Cancelled>(events.back())) {
      events.pop_back();
    }
  }

 private:
  ReferenceBook book_;
};

// Whether the Audit finds a problem when the commands run on a fresh Book.
template <typename Book>
bool audit_fails(const std::vector<Command>& commands) {
  Book book;
  Audit audit;
  std::vector<Event> events;
  for (const Command& command : commands) {
    events.clear();
    book.apply(command, events);
    audit.record(command, events);
    if (!audit.ok()) return true;
  }
  return false;
}

std::vector<Command> random_commands(std::uint64_t seed, int count, std::size_t target_resting) {
  RandomFlow flow(seed, FlowOptions{.target_resting = target_resting});
  std::vector<Command> commands;
  for (int i = 0; i < count; ++i) commands.push_back(flow.next());
  return commands;
}

template <typename Book>
void expect_one_minimal(const std::vector<Command>& shrunk) {
  ASSERT_TRUE(audit_fails<Book>(shrunk));
  for (std::size_t i = 0; i < shrunk.size(); ++i) {
    std::vector<Command> fewer = shrunk;
    fewer.erase(fewer.begin() + static_cast<std::ptrdiff_t>(i));
    EXPECT_FALSE(audit_fails<Book>(fewer)) << "still fails without command " << i;
  }
}

TEST(Minimize, ShrinksARandomRunThatCatchesAPriceBugToThreeCommands) {
  const std::vector<Command> commands = random_commands(12, 3'000, 100);
  ASSERT_TRUE(audit_fails<BookWithAPriceBug>(commands));
  ASSERT_FALSE(audit_fails<ReferenceBook>(commands));
  const std::vector<Command> shrunk = minimize(commands, audit_fails<BookWithAPriceBug>);
  ASSERT_EQ(shrunk.size(), 3U) << ::testing::PrintToString(lines(shrunk));
  // A resting order, then two takers on the other side.
  const auto& maker = std::get<NewOrder>(shrunk[0]);
  EXPECT_EQ(maker.type, OrderType::Limit);
  for (std::size_t i = 1; i < 3; ++i) EXPECT_EQ(std::get<NewOrder>(shrunk[i]).side, opposite(maker.side));
  expect_one_minimal<BookWithAPriceBug>(shrunk);
}

TEST(Minimize, ShrinksARandomRunThatCatchesAMissingCancelToOneCommand) {
  // A nearly empty book: with 100 orders resting, no market order in 3,000 commands went unfilled, so the bug
  // never showed.
  const std::vector<Command> commands = random_commands(13, 3'000, 5);
  ASSERT_TRUE(audit_fails<BookThatDropsAnUnfilledMarketOrder>(commands));
  const std::vector<Command> shrunk = minimize(commands, audit_fails<BookThatDropsAnUnfilledMarketOrder>);
  ASSERT_EQ(shrunk.size(), 1U) << ::testing::PrintToString(lines(shrunk));
  EXPECT_EQ(std::get<NewOrder>(shrunk[0]).type, OrderType::Market);
}

}  // namespace
}  // namespace orderbook
