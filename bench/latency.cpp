// orderbook_latency: latency percentiles, where the Google Benchmark suite gives throughput (`make latency`).
//
// 1. One thread, each command timed alone (a clock read before and after `apply`), for the reference model, day 2's
//    engine and the engine, on the benchmarks' books: RandomFlow seed 42, a warm-up to a target size, then the timed
//    commands.
// 2. The engine on its own thread (EngineThread), timed from the sender's side: from sending a command to receiving
//    its last event. One at a time (the round trip through both rings, nothing queued); paced, a command every 1/rate
//    seconds, timed from when it was due rather than when it went out, so a sender held up by a full ring counts the
//    wait (no coordinated omission); and saturated, sending as fast as the ring takes them, which gives the
//    throughput and the queueing delay at it.
//
// Usage: orderbook_latency [COMMANDS] [--csv FILE] [--trace FILE]   (1,000,000 timed commands by default)
//
// With --csv, each row's histogram goes to FILE too (bench/latency_histogram.hpp), for scripts/plot_latency.py; its
// `size` column is the number of orders resting after the row's warm-up. With --trace, each paced and saturated row
// of the engine's thread writes a line a command, `row,resting,index,due_ns,latency_ns` (due_ns from the row's
// start: when the command was due, or when it was sent for the saturated rows). Every figure is in nanoseconds, from std::chrono::steady_clock, whose step and cost are printed first: on Apple
// silicon it counts a 24 MHz timer, so a single command (tens of nanoseconds) shows as 0, 41 or 42 ns, and only the
// tail percentiles span several steps.

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

#include "latency_histogram.hpp"
#include "orderbook/book.hpp"
#include "orderbook/engine_thread.hpp"
#include "orderbook/map_book.hpp"
#include "orderbook/random_flow.hpp"
#include "orderbook/reference_book.hpp"

namespace orderbook {
namespace {

using Clock = std::chrono::steady_clock;

std::int64_t nanoseconds(Clock::duration d) { return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count(); }

// Room for any book these workloads build, so the engine never grows mid-run.
constexpr BookCapacity kRoom{.orders = 32'768, .levels = 8'192};

struct Workload {
  std::size_t resting = 0;  // orders resting after the warm-up, the same on every book: the CSV's `size`
  std::vector<Command> warm_up;
  std::vector<Command> timed;
};

// The benchmarks' workload (bench/book_bench.cpp), with as many timed commands as asked.
Workload workload(std::size_t target, std::size_t count) {
  RandomFlow flow(42, FlowOptions{.target_resting = target});
  Workload load;
  for (std::size_t i = 0; i < target * 4 + 1'000; ++i) load.warm_up.push_back(flow.next());
  for (std::size_t i = 0; i < count; ++i) load.timed.push_back(flow.next());
  Book book(kRoom);
  std::vector<Event> events;
  for (const Command& command : load.warm_up) {
    events.clear();
    book.apply(command, events);
  }
  load.resting = book.size();
  return load;
}

void print_header(const char* first) {
  std::printf("%-26s %8s %7s %7s %7s %7s %8s %8s %8s\n", first, "resting", "p50", "p90", "p99", "p99.9", "p99.99", "max",
              "mean");
}

// Where --csv writes each row's histogram (nowhere without it), and --trace the paced rows' commands.
latency::CsvWriter csv;
std::FILE* trace = nullptr;

// Sorts the samples, prints the row and writes its histogram. A row's `resting` is the orders resting after its
// warm-up, the book its timed commands start against, in the table and the CSV alike. (Until v1.0.0 the engine
// thread's rows printed the book at the end of the row instead: 66, 562 and 4,796 for 70, 583 and 5,410.)
void print_row(const char* section, const std::string& name, const Workload& load, std::vector<std::int64_t>& samples) {
  const latency::Summary s = latency::summarize(samples);
  std::printf("%-26s %8zu %7" PRId64 " %7" PRId64 " %7" PRId64 " %7" PRId64 " %8" PRId64 " %8" PRId64 " %8.1f\n",
              name.c_str(), load.resting, s.p50, s.p90, s.p99, s.p999, s.p9999, s.max, s.mean);
  csv.write(section, name, load.resting, samples);
}

void clock_report() {
  constexpr int kPairs = 1'000'000;
  std::vector<std::int64_t> deltas(kPairs);
  for (int i = 0; i < kPairs; ++i) {
    const auto a = Clock::now();
    const auto b = Clock::now();
    deltas[static_cast<std::size_t>(i)] = nanoseconds(b - a);
  }
  std::int64_t step = 0;
  for (const std::int64_t d : deltas) {
    if (d > 0 && (step == 0 || d < step)) step = d;
  }
  // The cost of a read: many reads in a row, timed together.
  constexpr int kReads = 10'000'000;
  const auto start = Clock::now();
  Clock::time_point last{};
  for (int i = 0; i < kReads; ++i) last = Clock::now();
  const double per_read = static_cast<double>(nanoseconds(last - start)) / kReads;
  std::printf("clock: steady_clock, smallest step seen %" PRId64 " ns, %.1f ns a read (%d reads in a row)\n\n", step,
              per_read, kReads);
}

template <typename AnyBook>
void one_thread(const char* name, AnyBook book, const Workload& load) {
  std::vector<Event> events;
  events.reserve(1 << 16);
  for (const Command& command : load.warm_up) {
    events.clear();
    book.apply(command, events);
  }
  if (book.size() != load.resting) {  // every book rests the same orders after the same commands
    std::fprintf(stderr, "orderbook_latency: %s rests %zu orders after the warm-up, the engine %zu\n", name, book.size(),
                 load.resting);
    std::exit(1);
  }
  std::vector<std::int64_t> samples(load.timed.size());
  for (std::size_t i = 0; i < load.timed.size(); ++i) {
    events.clear();
    const auto start = Clock::now();
    book.apply(load.timed[i], events);
    const auto end = Clock::now();
    samples[i] = nanoseconds(end - start);
  }
  print_row("one thread", name, load, samples);
}

// Sends every command, receiving while it waits, untimed.
void send_untimed(EngineThread& engine, const std::vector<Command>& commands, std::uint64_t& sequence) {
  std::uint64_t done = sequence;
  const std::uint64_t end = sequence + commands.size();
  std::size_t next = 0;
  Report report;
  while (done < end) {
    if (next < commands.size() && engine.try_send(Request{sequence, commands[next]})) {
      ++sequence;
      ++next;
    }
    while (engine.try_receive(report)) {
      if (report.last) ++done;
    }
  }
}

void one_at_a_time(const Workload& load) {
  EngineThread engine(kRoom);
  std::uint64_t sequence = 0;
  send_untimed(engine, load.warm_up, sequence);
  std::vector<std::int64_t> samples(load.timed.size());
  Report report;
  for (std::size_t i = 0; i < load.timed.size(); ++i) {
    const auto start = Clock::now();
    while (!engine.try_send(Request{sequence, load.timed[i]})) cpu_relax();
    for (;;) {
      if (!engine.try_receive(report)) {
        cpu_relax();
        continue;
      }
      if (report.last) break;
    }
    samples[i] = nanoseconds(Clock::now() - start);
    ++sequence;
  }
  engine.stop();
  print_row("engine thread", "one at a time", load, samples);
}

// rate 0: as fast as the ring takes them. Otherwise command i is due at start + i / rate, and is timed from then.
void paced(const Workload& load, double rate) {
  EngineThread engine(kRoom);
  std::uint64_t sequence = 0;
  send_untimed(engine, load.warm_up, sequence);
  const std::uint64_t first = sequence;
  const std::size_t count = load.timed.size();
  std::vector<Clock::time_point> due(count);
  std::vector<std::int64_t> samples(count);
  std::size_t next = 0;
  std::size_t done = 0;
  std::size_t late = 0;  // paced commands sent more than 5 µs after they were due: the sender was held up
  Report report;
  const auto start = Clock::now();
  const auto interval = std::chrono::duration<double, std::nano>(rate > 0 ? 1e9 / rate : 0);
  while (done < count) {
    if (next < count) {
      const auto now = Clock::now();
      const auto when = start + std::chrono::duration_cast<Clock::duration>(interval * static_cast<double>(next));
      if (now >= when && engine.try_send(Request{first + next, load.timed[next]})) {
        due[next] = rate > 0 ? when : now;
        if (rate > 0 && nanoseconds(now - when) > 5'000) ++late;
        ++next;
      }
    }
    while (engine.try_receive(report)) {
      if (!report.last) continue;
      const std::size_t i = report.sequence - first;
      samples[i] = nanoseconds(Clock::now() - due[i]);
      ++done;
    }
  }
  const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
  engine.stop();
  // How much of the tail the sender's own delays explain: commands sent late, against commands slower than 5 µs.
  const auto slow = static_cast<std::size_t>(
      std::count_if(samples.begin(), samples.end(), [](std::int64_t s) { return s > 5'000; }));
  char name[64];
  char row[32];  // the CSV's name for the row, without the rate it got
  if (rate > 0) {
    std::snprintf(name, sizeof name, "paced %.0fM/s (got %.2fM/s)", rate / 1e6, static_cast<double>(count) / seconds / 1e6);
    std::snprintf(row, sizeof row, "paced %.0fM/s", rate / 1e6);
  } else {
    std::snprintf(name, sizeof name, "saturated (%.2fM/s)", static_cast<double>(count) / seconds / 1e6);
    std::snprintf(row, sizeof row, "saturated");
  }
  if (trace != nullptr) {  // before summarize sorts the samples
    for (std::size_t i = 0; i < count; ++i) {
      std::fprintf(trace, "%s,%zu,%zu,%" PRId64 ",%" PRId64 "\n", row, load.resting, i, nanoseconds(due[i] - start),
                   samples[i]);
    }
    if (std::fflush(trace) != 0) {
      std::fprintf(stderr, "orderbook_latency: writing the trace failed\n");
      std::exit(1);
    }
  }
  const latency::Summary s = latency::summarize(samples);
  std::printf("%-26s %8zu %7" PRId64 " %7" PRId64 " %7" PRId64 " %7" PRId64 " %8" PRId64 " %8" PRId64 " %8.1f\n",
              name, load.resting, s.p50, s.p90, s.p99, s.p999, s.p9999, s.max, s.mean);
  csv.write("engine thread", row, load.resting, samples);
  if (rate > 0) std::printf("%26s sent over 5 us late: %zu; slower than 5 us: %zu\n", "", late, slow);
}

}  // namespace
}  // namespace orderbook

int main(int argc, char** argv) {
  using namespace orderbook;
  std::size_t count = 1'000'000;
  const char* csv_path = nullptr;
  const char* trace_path = nullptr;
  bool counted = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--csv" && i + 1 < argc && csv_path == nullptr) {
      csv_path = argv[++i];
    } else if (arg == "--trace" && i + 1 < argc && trace_path == nullptr) {
      trace_path = argv[++i];
    } else if (!counted && !arg.empty() && arg[0] != '-' && (count = std::strtoull(arg.c_str(), nullptr, 10)) != 0) {
      counted = true;
    } else {
      std::fprintf(stderr, "usage: orderbook_latency [COMMANDS] [--csv FILE] [--trace FILE]\n");
      return 2;
    }
  }
  if (csv_path != nullptr) {
    try {
      csv.open(csv_path);
    } catch (const std::exception& error) {
      std::fprintf(stderr, "orderbook_latency: %s\n", error.what());
      return 1;
    }
  }
  if (trace_path != nullptr) {
    trace = std::fopen(trace_path, "w");
    if (trace == nullptr) {
      std::fprintf(stderr, "orderbook_latency: can't write %s\n", trace_path);
      return 1;
    }
    std::fputs("row,resting,index,due_ns,latency_ns\n", trace);
  }
  std::printf("orderbook_latency: %zu timed commands a row, RandomFlow seed 42 (synthetic)\n", count);
  clock_report();

  std::printf("One thread, each command timed alone (ns, with one clock read):\n");
  print_header("book");
  for (const std::size_t target : {std::size_t{100}, std::size_t{1'000}, std::size_t{10'000}}) {
    const Workload load = workload(target, count);
    one_thread("reference model", ReferenceBook{}, load);
    one_thread("day 2's engine (MapBook)", MapBook{}, load);
    one_thread("engine", Book(kRoom), load);
  }

  std::printf("\nThe engine on its own thread, from sending a command to receiving its last event (ns):\n");
  print_header("mode");
  for (const std::size_t target : {std::size_t{100}, std::size_t{1'000}, std::size_t{10'000}}) {
    const Workload load = workload(target, count);
    one_at_a_time(load);
    paced(load, 1e6);
    paced(load, 5e6);
    paced(load, 0);
  }
  return 0;
}
