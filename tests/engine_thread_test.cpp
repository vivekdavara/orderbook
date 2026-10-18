// The engine on its own thread: what comes back through the rings is exactly what the book gives when called
// directly, command for command, and stopping lets it finish what was sent, or gives up on a sender that stopped
// receiving. `make tsan` runs these under ThreadSanitizer too.

#include "orderbook/engine_thread.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "orderbook/differential.hpp"
#include "orderbook/random_flow.hpp"
#include "orderbook/text.hpp"

namespace orderbook {
namespace {

// Sends every command in turn, receiving while it waits, and returns each command's events in order. Fails the test
// if a report is out of order, or if the commands aren't all done within a minute (say, because a command's last
// event wasn't flagged).
std::vector<std::vector<Event>> send_all(EngineThread& engine, const std::vector<Command>& commands) {
  std::vector<std::vector<Event>> answers(commands.size());
  std::uint64_t next_to_send = 0;
  std::uint64_t done = 0;  // commands whose last event has come back
  std::size_t problems = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(1);
  while (done < commands.size()) {
    if (std::chrono::steady_clock::now() > deadline) {
      ADD_FAILURE() << "after a minute, " << done << " of " << commands.size() << " commands done";
      break;
    }
    if (next_to_send < commands.size() && engine.try_send(Request{next_to_send, commands[next_to_send]})) {
      ++next_to_send;
    }
    Report report;
    while (engine.try_receive(report)) {
      if (report.sequence != done) {
        ++problems;
        if (problems < 5) ADD_FAILURE() << "a report for command " << report.sequence << " while " << done << " was open";
        continue;
      }
      answers[report.sequence].push_back(report.event);
      if (report.last) ++done;
    }
  }
  return answers;
}

std::vector<Command> stream(std::uint64_t seed, FlowOptions options, std::size_t count) {
  RandomFlow flow(seed, options);
  std::vector<Command> commands;
  for (std::size_t i = 0; i < count; ++i) commands.push_back(flow.next());
  return commands;
}

TEST(EngineThread, AnswersExactlyAsTheBookDoesWhenCalledDirectly) {
  for (const auto& [seed, options] : {std::pair{1ULL, FlowOptions{}},
                                      std::pair{2ULL, FlowOptions{.price_range = 2'000, .target_resting = 3'000}},
                                      std::pair{3ULL, FlowOptions{.price_range = 3, .max_quantity = 20}}}) {
    const std::vector<Command> commands = stream(seed, options, 100'000);
    EngineThread engine;
    const std::vector<std::vector<Event>> answers = send_all(engine, commands);
    engine.stop();
    Book direct;
    std::vector<Event> events;
    for (std::size_t i = 0; i < commands.size(); ++i) {
      events.clear();
      direct.apply(commands[i], events);
      ASSERT_EQ(format_events(answers[i]), format_events(events)) << "seed " << seed << ", command " << i;
    }
    EXPECT_EQ(state_difference(direct, engine.book()), std::nullopt) << "seed " << seed;
    EXPECT_EQ(engine.applied(), commands.size());
  }
}

TEST(EngineThread, StoppingLetsItApplyEverythingAlreadySent) {
  EngineThread engine;
  // 1,000 bids at one price: each is accepted and rests, one event each, so the reports fit in their ring.
  for (std::uint64_t id = 1; id <= 1'000; ++id) {
    const NewOrder order{OrderId{id}, Side::Buy, OrderType::Limit, Quantity{1}, Price{100}};
    ASSERT_TRUE(engine.try_send(Request{id, order}));
  }
  engine.stop();
  EXPECT_EQ(engine.applied(), 1'000U);
  EXPECT_EQ(engine.book().size(), 1'000U);
  EXPECT_EQ(format(engine.book().depth(Side::Buy)), "100x1000");
  // Its reports are still in the ring, in order.
  Report report;
  for (std::uint64_t id = 1; id <= 1'000; ++id) {
    ASSERT_TRUE(engine.try_receive(report));
    EXPECT_EQ(report.sequence, id);
    EXPECT_EQ(format(report.event), "accepted " + std::to_string(id));
    EXPECT_TRUE(report.last);
  }
  EXPECT_FALSE(engine.try_receive(report));
  engine.stop();  // again: nothing to do
}

TEST(EngineThread, StoppingGivesUpOnASenderThatStoppedReceiving) {
  EngineThread engine;
  // Single-event commands, and nothing received. The engine fills the ring of reports with the first kRingSize
  // answers, applies one more and waits to push its report; then kRingSize more requests fill their ring. Only then
  // can 2 kRingSize + 1 sends have gone in, so when this loop ends the engine is waiting on a full ring of reports.
  constexpr std::uint64_t kRing = EngineThread::kRingSize;
  std::uint64_t sent = 0;
  while (sent < 2 * kRing + 1) {
    const NewOrder order{OrderId{sent + 1}, Side::Buy, OrderType::Limit, Quantity{1}, Price{100}};
    if (engine.try_send(Request{sent + 1, order})) {
      ++sent;
    } else {
      cpu_relax();
    }
  }
  engine.stop();  // must return, though the engine was waiting for room no one would make
  EXPECT_EQ(engine.applied(), kRing + 1);
  EXPECT_EQ(engine.book().size(), kRing + 1);
}

TEST(EngineThread, DestroyingARunningEngineStopsIt) {
  auto engine = std::make_unique<EngineThread>();
  ASSERT_TRUE(engine->try_send(Request{1, CancelOrder{OrderId{9}}}));
  engine.reset();  // joins; a thread left running would terminate the test binary
  SUCCEED();
}

}  // namespace
}  // namespace orderbook
