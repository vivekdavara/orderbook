// orderbook: a command-line tool over the engine and the test kit.
//
//   orderbook [--book B] check FILE...           run scenario files; exit 1 if any doesn't match
//   orderbook [--book B] run [FILE]              apply commands (from FILE or stdin) and print a scenario transcript
//   orderbook gen SEED COUNT [opts]              print COUNT random commands, one "> ..." line each
//   orderbook [--book B] audit SEED COUNT [opts] run a random stream with every invariant check on; print a summary
//   orderbook diff SEED COUNT [opts] [--every N] run a random stream through the reference model and the engine,
//                                                comparing every event, and their states every N commands (1,000)
//
// B is `engine` (the default) or `reference`. The options: --target N, --range N, --max-quantity N, --invalid
// PER_MILLE (RandomFlow's options). `orderbook gen 1 20 | orderbook run` prints what the engine does with a stream;
// the transcript is itself a scenario file, so `orderbook check` passes it, on either book. When diff finds a
// difference it prints the shrunk commands as a scenario file and exits 1: `orderbook check` of that file fails on
// the engine and passes on the reference model.

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include "orderbook/book.hpp"
#include "orderbook/check.hpp"
#include "orderbook/differential.hpp"
#include "orderbook/random_flow.hpp"
#include "orderbook/reference_book.hpp"
#include "orderbook/scenario.hpp"
#include "orderbook/text.hpp"

// The engine the tool runs. A test build (tests/CMakeLists.txt) puts a deliberately broken book here, so that the CLI
// tests can check the tool's own failure paths: diff's report, its exit code and the scenario it prints, and that
// --book runs the book it names.
#ifdef ORDERBOOK_CLI_ENGINE_HEADER
#include ORDERBOOK_CLI_ENGINE_HEADER
#endif

namespace {

using namespace orderbook;

#ifdef ORDERBOOK_CLI_ENGINE
using Engine = ORDERBOOK_CLI_ENGINE;
#else
using Engine = Book;
#endif

constexpr int kUsageError = 2;

int usage() {
  std::cerr << "usage: orderbook [--book engine|reference] check FILE...\n"
               "       orderbook [--book engine|reference] run [FILE]\n"
               "       orderbook gen SEED COUNT [--target N] [--range N] [--max-quantity N] [--invalid PER_MILLE]\n"
               "       orderbook [--book engine|reference] audit SEED COUNT [same options as gen]\n"
               "       orderbook diff SEED COUNT [same options as gen] [--every N]\n";
  return kUsageError;
}

// Through rdbuf() rather than istreambuf_iterator: with the iterator, GCC 14 at -O3 reports a "potential null pointer
// dereference" inside libstdc++'s streambuf (a false positive that -Werror makes fatal).
std::optional<std::string> read_all(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

template <typename Int>
std::optional<Int> to_int(std::string_view text) {
  Int value{};
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || ec != std::errc{} || ptr != text.data() + text.size()) return std::nullopt;
  return value;
}

template <typename AnyBook>
int check(const std::vector<std::string>& files) {
  if (files.empty()) return usage();
  int failed = 0;
  for (const std::string& file : files) {
    const auto text = read_all(file);
    if (!text) {
      std::cerr << file << ": can't read it\n";
      ++failed;
      continue;
    }
    std::string error;
    const auto scenario = parse_scenario(*text, &error);
    if (!scenario) {
      std::cerr << file << ": " << error << "\n";
      ++failed;
      continue;
    }
    AnyBook book;
    const std::string mismatch = run_scenario(*scenario, book);
    if (mismatch.empty()) {
      std::cout << "ok " << file << " (" << scenario->size() << " commands)\n";
    } else {
      std::cerr << file << ": " << mismatch << (mismatch.back() == '\n' ? "" : "\n");
      ++failed;
    }
  }
  return failed == 0 ? 0 : 1;
}

template <typename AnyBook>
int run(std::istream& in) {
  AnyBook book;
  std::vector<Event> events;
  std::string line;
  int number = 0;
  int commands = 0;
  while (std::getline(in, line)) {
    ++number;
    std::string_view text = line;
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    if (text.empty() || text.front() == '#') continue;
    if (text.front() == '>') text.remove_prefix(1);
    std::string error;
    const auto command = parse_command(text, &error);
    if (!command) {
      std::cerr << "line " << number << ": " << error << "\n";
      return 1;
    }
    events.clear();
    book.apply(*command, events);
    ++commands;
    std::cout << "> " << format(*command) << "\n";
    for (const Event& event : events) std::cout << format(event) << "\n";
  }
  // A scenario file's depth and state checks belong to a command, so with no commands there is nothing to print: an
  // empty transcript is an empty scenario file, which check passes.
  if (commands > 0) std::cout << state_lines(book);
  return 0;
}

struct FlowArgs {
  std::uint64_t seed = 0;
  std::uint64_t count = 0;
  FlowOptions options;
  std::uint64_t every = 1000;  // diff's --every
};

std::optional<FlowArgs> parse_flow_args(const std::vector<std::string>& args, bool takes_every) {
  if (args.size() < 2) return std::nullopt;
  FlowArgs parsed;
  const auto seed = to_int<std::uint64_t>(args[0]);
  const auto count = to_int<std::uint64_t>(args[1]);
  if (!seed || !count) return std::nullopt;
  parsed.seed = *seed;
  parsed.count = *count;
  for (std::size_t i = 2; i < args.size(); i += 2) {
    if (i + 1 >= args.size()) return std::nullopt;
    const std::string& flag = args[i];
    const auto value = to_int<std::int64_t>(args[i + 1]);
    if (!value || *value < 0) return std::nullopt;
    if (flag == "--target") {
      parsed.options.target_resting = static_cast<std::size_t>(*value);
    } else if (flag == "--range") {
      parsed.options.price_range = *value;
    } else if (flag == "--max-quantity" && *value >= 1) {
      parsed.options.max_quantity = *value;
    } else if (flag == "--invalid" && *value <= 1000) {
      parsed.options.invalid_per_mille = static_cast<int>(*value);
    } else if (flag == "--every" && takes_every && *value >= 1) {
      parsed.every = static_cast<std::uint64_t>(*value);
    } else {
      return std::nullopt;
    }
  }
  return parsed;
}

int gen(const FlowArgs& args) {
  RandomFlow flow(args.seed, args.options);
  for (std::uint64_t i = 0; i < args.count; ++i) std::cout << "> " << format(flow.next()) << "\n";
  return 0;
}

template <typename AnyBook>
int audit(const FlowArgs& args) {
  RandomFlow flow(args.seed, args.options);
  AnyBook book;
  Audit checker;
  std::vector<Event> events;
  std::array<std::uint64_t, 4> kinds{};  // accepted or rejected, trades, cancelled; indexed by the variant
  std::uint64_t largest = 0;
  std::uint64_t ran = 0;
  bool sound = true;  // no check_book problem at any checkpoint, not only the last
  while (ran < args.count) {
    const Command command = flow.next();
    events.clear();
    book.apply(command, events);
    checker.record(command, events);
    ++ran;
    for (const Event& event : events) ++kinds[event.index()];
    if (ran % 1000 == 0 || ran == args.count) {
      for (const std::string& problem : check_book(book)) {
        std::cerr << "command " << ran << ": " << problem << "\n";
        sound = false;
      }
      checker.compare(book);
    }
    largest = std::max<std::uint64_t>(largest, book.size());
    if (!checker.ok() || !sound) break;
  }
  for (const std::string& problem : checker.problems()) std::cerr << problem << "\n";
  std::cout << "commands " << ran << ", accepted " << kinds[0] << ", rejected " << kinds[1] << ", trades "
            << kinds[2] << ", cancelled " << kinds[3] << "; resting at the end " << book.size() << ", at most "
            << largest << "\n";
  return checker.ok() && sound ? 0 : 1;
}

// The reference model is the expected book and the engine the one under test. A difference is reported before the
// shrinking starts, since a book with a memory error can crash during it.
int diff(const FlowArgs& args) {
  const StreamDifference result = diff_random_stream<ReferenceBook, Engine>(args.seed, args.options, args.count,
                                                                         args.every);
  if (result.difference) {
    std::cerr << "the engine differs from the reference model: " << result.difference->what << "\n"
              << "shrinking the " << result.commands << " commands to the ones it needs\n"
              << std::flush;
    std::cout << shrink_random_stream<ReferenceBook, Engine>(args.seed, args.options, result.commands);
    return 1;
  }
  std::cout << "commands " << result.commands << ": the same events from both books, and the same state every "
            << args.every << " commands and at the end\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  bool engine = true;
  bool book_named = false;
  if (!args.empty() && args[0] == "--book") {
    if (args.size() < 2 || (args[1] != "engine" && args[1] != "reference")) return usage();
    engine = args[1] == "engine";
    book_named = true;
    args.erase(args.begin(), args.begin() + 2);
  }
  if (args.empty()) return usage();
  const std::string& verb = args[0];
  const std::vector<std::string> rest(args.begin() + 1, args.end());
  if (verb == "check") return engine ? check<Engine>(rest) : check<ReferenceBook>(rest);
  if (verb == "run") {
    if (rest.empty()) return engine ? run<Engine>(std::cin) : run<ReferenceBook>(std::cin);
    if (rest.size() > 1) return usage();
    std::ifstream in(rest[0]);
    if (!in) {
      std::cerr << rest[0] << ": can't read it\n";
      return 1;
    }
    return engine ? run<Engine>(in) : run<ReferenceBook>(in);
  }
  if (verb == "gen" || verb == "audit" || verb == "diff") {
    const auto flow_args = parse_flow_args(rest, verb == "diff");
    // gen uses no book and diff uses both, so naming one is a mistake.
    if (!flow_args || (book_named && verb != "audit")) return usage();
    try {
      if (verb == "gen") return gen(*flow_args);
      if (verb == "diff") return diff(*flow_args);
      return engine ? audit<Engine>(*flow_args) : audit<ReferenceBook>(*flow_args);
    } catch (const std::invalid_argument& error) {  // options RandomFlow can't honour
      std::cerr << "orderbook: " << error.what() << "\n";
      return kUsageError;
    }
  }
  return usage();
}
