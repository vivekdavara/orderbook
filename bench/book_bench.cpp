// Benchmarks: the random command stream, and the throughput of both books on the same commands against the same
// book, so the engine's numbers stand beside the reference model's (the baseline from day 1). Run them from the
// release build: `make bench`.

#include <benchmark/benchmark.h>

#include <cstddef>
#include <vector>

#include "orderbook/book.hpp"
#include "orderbook/random_flow.hpp"
#include "orderbook/reference_book.hpp"

namespace orderbook {
namespace {

// A book's commands for one benchmark: the warm-up that fills a book to about the target size (below the target,
// new limit orders outnumber cancels by 35 in 100), then the 10,000 commands that are timed.
struct Workload {
  std::vector<Command> warm_up;
  std::vector<Command> timed;
};

Workload workload(std::size_t target) {
  RandomFlow flow(42, FlowOptions{.target_resting = target});
  Workload load;
  for (std::size_t i = 0; i < target * 4 + 1'000; ++i) load.warm_up.push_back(flow.next());
  constexpr std::size_t kCommands = 10'000;
  for (std::size_t i = 0; i < kCommands; ++i) load.timed.push_back(flow.next());
  return load;
}

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
  const Workload load = workload(static_cast<std::size_t>(state.range(0)));
  ReferenceBook warmed;
  std::vector<Event> events;
  for (const Command& command : load.warm_up) {
    events.clear();
    warmed.apply(command, events);
  }
  // The same commands against a fresh copy of the warmed book each iteration.
  for (auto _ : state) {
    state.PauseTiming();
    ReferenceBook book = warmed;
    state.ResumeTiming();
    for (const Command& command : load.timed) {
      events.clear();
      book.apply(command, events);
    }
    benchmark::DoNotOptimize(events.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<benchmark::IterationCount>(load.timed.size()));
  state.counters["resting"] = static_cast<double>(warmed.size());
}
BENCHMARK(BM_ReferenceBook)->Arg(10)->Arg(100)->Arg(200)->Arg(400)->Arg(1'000)->Arg(10'000)->Unit(benchmark::kMillisecond);

// The engine on exactly the reference model's commands and book (the differential tests say the books are the
// same after every command). A Book can't be copied, since its index points into its own containers, so each
// iteration builds its book again from the warm-up, outside the timing.
void BM_Engine(benchmark::State& state) {
  const Workload load = workload(static_cast<std::size_t>(state.range(0)));
  std::vector<Event> events;
  std::size_t resting = 0;
  for (auto _ : state) {
    state.PauseTiming();
    Book book;
    for (const Command& command : load.warm_up) {
      events.clear();
      book.apply(command, events);
    }
    resting = book.size();
    state.ResumeTiming();
    for (const Command& command : load.timed) {
      events.clear();
      book.apply(command, events);
    }
    benchmark::DoNotOptimize(events.data());
    benchmark::ClobberMemory();
    state.PauseTiming();  // the book's destruction isn't timed either
    book = Book();
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<benchmark::IterationCount>(load.timed.size()));
  state.counters["resting"] = static_cast<double>(resting);
}
BENCHMARK(BM_Engine)->Arg(10)->Arg(100)->Arg(200)->Arg(400)->Arg(1'000)->Arg(10'000)->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace orderbook
