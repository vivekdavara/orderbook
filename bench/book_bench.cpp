// Benchmarks: the random command stream, the throughput of each book on the same commands against the same book, so
// the engine's numbers stand beside the reference model's (day 1's baseline) and day 2's engine's (`MapBook`, the
// baseline for day 3's), and the SPSC ring on one thread and two. Run them from the release build: `make bench`.

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "orderbook/book.hpp"
#include "orderbook/engine_thread.hpp"
#include "orderbook/map_book.hpp"
#include "orderbook/random_flow.hpp"
#include "orderbook/reference_book.hpp"
#include "orderbook/spsc_ring.hpp"

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

// A new, empty book. Day 3's engine is built with room for every book these workloads make (the largest holds about
// 5,500 orders), so it never grows in the timed part; it would grow a few times past its default room otherwise.
template <typename Engine>
Engine fresh_book() {
  return Engine();
}
template <>
Book fresh_book<Book>() {
  return Book(BookCapacity{.orders = 32'768, .levels = 8'192});
}

// An engine on exactly the reference model's commands and book (the differential tests say the books are the same
// after every command). Day 2's engine can't be copied, since its index points into its own containers, so for both
// engines each iteration builds its book again from the warm-up, outside the timing.
template <typename Engine>
void BM_EngineOf(benchmark::State& state) {
  const Workload load = workload(static_cast<std::size_t>(state.range(0)));
  std::vector<Event> events;
  std::size_t resting = 0;
  for (auto _ : state) {
    state.PauseTiming();
    Engine book = fresh_book<Engine>();
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
    book = Engine();
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<benchmark::IterationCount>(load.timed.size()));
  state.counters["resting"] = static_cast<double>(resting);
}

// Day 3's engine, and day 2's (std::map of std::list levels, std::unordered_map index), on the same work.
void BM_Engine(benchmark::State& state) { BM_EngineOf<Book>(state); }
void BM_MapBook(benchmark::State& state) { BM_EngineOf<MapBook>(state); }
BENCHMARK(BM_Engine)->Arg(10)->Arg(100)->Arg(200)->Arg(400)->Arg(1'000)->Arg(10'000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_MapBook)->Arg(10)->Arg(100)->Arg(200)->Arg(400)->Arg(1'000)->Arg(10'000)->Unit(benchmark::kMillisecond);

// The ring's own cost on one thread: a push and a pop of one value, the ring never full or empty for long.
template <typename T>
void BM_SpscRingPushPop(benchmark::State& state) {
  auto ring = std::make_unique<SpscRing<T, 1024>>();
  T value{};
  T out{};
  for (auto _ : state) {
    ring->try_push(value);
    ring->try_pop(out);
    benchmark::DoNotOptimize(out);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscRingPushPop<std::uint64_t>);
BENCHMARK(BM_SpscRingPushPop<Request>);

// Two threads: this one pushes 2^20 values through a ring of 1,024 while another pops them, spinning when the ring is
// full or empty. Wall time, since the work is on two threads; the threads aren't pinned (macOS can't).
template <typename T>
void BM_SpscRingTwoThreads(benchmark::State& state) {
  constexpr std::size_t kValues = std::size_t{1} << 20;
  auto ring = std::make_unique<SpscRing<T, 1024>>();
  for (auto _ : state) {
    std::thread consumer([&ring] {
      T out{};
      for (std::size_t got = 0; got < kValues;) {
        if (ring->try_pop(out)) {
          ++got;
        } else {
          cpu_relax();
        }
      }
      benchmark::DoNotOptimize(out);
    });
    const T value{};
    for (std::size_t i = 0; i < kValues; ++i) {
      while (!ring->try_push(value)) cpu_relax();
    }
    consumer.join();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<benchmark::IterationCount>(kValues));
}
// Values of a given size, to see how the two-thread rate depends on how many slots share a cache line.
template <std::size_t Bytes>
struct Payload {
  std::uint64_t words[Bytes / 8] = {};
};

BENCHMARK(BM_SpscRingTwoThreads<std::uint64_t>)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_SpscRingTwoThreads<Payload<16>>)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_SpscRingTwoThreads<Payload<32>>)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_SpscRingTwoThreads<Request>)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_SpscRingTwoThreads<Payload<64>>)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_SpscRingTwoThreads<Payload<128>>)->UseRealTime()->Unit(benchmark::kMillisecond);


}  // namespace
}  // namespace orderbook
