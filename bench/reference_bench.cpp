// Benchmarks of the test kit: the random command stream, and the reference model's throughput, which is the
// baseline the engine (day 2) has to beat. Run them from the release build: `make bench`.

#include <benchmark/benchmark.h>

#include <cstddef>
#include <vector>

#include "orderbook/random_flow.hpp"
#include "orderbook/reference_book.hpp"

namespace orderbook {
namespace {

// What generating one command costs. Benchmarks of a book generate their commands before timing.
void BM_RandomFlowNext(benchmark::State& state) {
  RandomFlow flow(1);
  for (auto _ : state) {
    Command command = flow.next();
    benchmark::DoNotOptimize(command);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RandomFlowNext);

// The reference model on a random stream with the book held near a given size (the argument). It scans a whole
// side for every fill and every cancel, so its throughput falls as the book grows.
void BM_ReferenceBook(benchmark::State& state) {
  const auto target = static_cast<std::size_t>(state.range(0));
  RandomFlow flow(42, FlowOptions{.target_resting = target});

  // Fill a book to its size first (below the target, new limit orders outnumber cancels by 35 in 100), then time
  // the same 10,000 commands against a fresh copy of it each iteration.
  ReferenceBook warmed;
  std::vector<Event> events;
  for (std::size_t i = 0; i < target * 4 + 1'000; ++i) {
    events.clear();
    warmed.apply(flow.next(), events);
  }
  constexpr std::size_t kCommands = 10'000;
  std::vector<Command> commands;
  commands.reserve(kCommands);
  for (std::size_t i = 0; i < kCommands; ++i) commands.push_back(flow.next());

  for (auto _ : state) {
    state.PauseTiming();
    ReferenceBook book = warmed;
    state.ResumeTiming();
    for (const Command& command : commands) {
      events.clear();
      book.apply(command, events);
    }
    benchmark::DoNotOptimize(events.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * static_cast<std::int64_t>(kCommands));
  state.counters["resting"] = static_cast<double>(warmed.size());
}
BENCHMARK(BM_ReferenceBook)->Arg(10)->Arg(100)->Arg(1'000)->Arg(10'000)->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace orderbook
