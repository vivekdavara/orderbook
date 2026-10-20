// orderbook: a command-line tool over the engine and the test kit.
//
//   orderbook [--book B] check FILE...           run scenario files; exit 1 if any doesn't match
//   orderbook [--book B] run [FILE]              apply commands (from FILE or stdin) and print a scenario transcript
//   orderbook gen SEED COUNT [opts]              print COUNT random commands, one "> ..." line each
//   orderbook [--book B] audit SEED COUNT [opts] run a random stream with every invariant check on; print a summary
//   orderbook diff SEED COUNT [opts] [--every N] run a random stream through the reference model and the engine,
//                                                comparing every event, and their states every N commands (1,000)
//   orderbook version                            print the version and how this binary was built
//   orderbook [--book B] replay [FILE] [--every N] [--levels K] [--updates]
//                                                replay commands (from FILE or stdin) through a book and the depth
//                                                feed built from its events; print an L2 snapshot (K levels a side,
//                                                10 by default) every N commands (1,000) and at the end, and with
//                                                --updates a line of each command's level updates
//
// B is `engine` (the default) or `reference`. The options: --target N, --range N, --max-quantity N, --invalid
// PER_MILLE (RandomFlow's options). `orderbook gen 1 20 | orderbook run` prints what the engine does with a stream;
// the transcript is itself a scenario file, so `orderbook check` passes it, on either book. When diff finds a
// difference it prints the shrunk commands as a scenario file and exits 1: `orderbook check` of that file fails on
// the engine and passes on the reference model. `orderbook gen 1 1000000 | orderbook replay --every 100000` replays a
// synthetic stream; each snapshot is checked against the book's own depth, and its timing goes to stderr, so the
// output is the same on every book and every run.

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
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
#include "orderbook/market_data.hpp"
#include "orderbook/random_flow.hpp"
#include "orderbook/reference_book.hpp"
#include "orderbook/scenario.hpp"
#include "orderbook/text.hpp"
#include "orderbook/version.hpp"

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
               "       orderbook diff SEED COUNT [same options as gen] [--every N]\n"
               "       orderbook [--book engine|reference] replay [FILE] [--every N] [--levels K] [--updates]\n"
               "       orderbook version\n";
  return kUsageError;
}

#ifndef ORDERBOOK_BUILD_TYPE
#define ORDERBOOK_BUILD_TYPE ""
#endif

// The version from CMakeLists.txt's project(), then the compiler, the build type and whether assertions are on.
int version() {
#if defined(__clang__)
  const char* compiler = "Clang " __clang_version__;
#elif defined(__GNUC__)
  const char* compiler = "GCC " __VERSION__;
#else
  const char* compiler = "an unknown compiler";
#endif
#ifdef NDEBUG
  const char* assertions = "assertions off";
#else
  const char* assertions = "assertions on";
#endif
  const std::string build_type = ORDERBOOK_BUILD_TYPE;
  std::cout << "orderbook " << kVersion << "\n"
            << "built with " << compiler << ", C++ " << __cplusplus << ", "
            << (build_type.empty() ? std::string() : build_type + ", ") << assertions << "\n";
  return 0;
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

// A line of commands for run or replay, trimmed and without the "> " that gen prints; empty for a blank line or a
// comment.
std::string_view command_text(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
  if (text.empty() || text.front() == '#') return {};
  if (text.front() == '>') text.remove_prefix(1);
  return text;
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
    const std::string_view text = command_text(line);
    if (text.empty()) continue;
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

struct ReplayArgs {
  std::optional<std::string> file;
  std::uint64_t every = 1'000;
  std::size_t levels = 10;
  bool updates = false;
};

std::optional<ReplayArgs> parse_replay_args(const std::vector<std::string>& args) {
  ReplayArgs parsed;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "--updates") {
      parsed.updates = true;
    } else if (arg == "--every" || arg == "--levels") {
      if (i + 1 >= args.size()) return std::nullopt;
      const auto value = to_int<std::uint64_t>(args[++i]);
      if (!value || *value == 0) return std::nullopt;
      if (arg == "--every") {
        parsed.every = *value;
      } else {
        parsed.levels = static_cast<std::size_t>(std::min<std::uint64_t>(*value, 1'000'000'000));
      }
    } else if (!arg.starts_with("--") && !parsed.file) {
      parsed.file = arg;
    } else {
      return std::nullopt;
    }
  }
  return parsed;
}

// The commands through a book and the depth feed that follows its events. A snapshot is printed every --every
// commands and after the last, each first checked against the book's own depth: a feed that disagrees with its book
// (or events the feed can't follow) is an error, exit 1.
template <typename AnyBook>
int replay(std::istream& in, const ReplayArgs& args) {
  AnyBook book;
  DepthFeed feed;
  std::vector<Event> events;
  std::vector<LevelUpdate> updates;
  std::uint64_t trades = 0;
  std::uint64_t changes = 0;
  std::uint64_t snapshots = 0;
  const auto snapshot = [&]() {
    const L2Snapshot top = feed.snapshot(args.levels);
    for (const Side side : {Side::Buy, Side::Sell}) {
      std::vector<Level> depth = book.depth(side);
      if (depth.size() > args.levels) depth.resize(args.levels);
      if (depth != (side == Side::Buy ? top.bids : top.asks)) {
        std::cerr << "after command " << top.sequence << " the feed's " << to_string(side) << " levels are '"
                  << format(side == Side::Buy ? top.bids : top.asks) << "' and the book's '" << format(depth)
                  << "'\n";
        return false;
      }
    }
    std::cout << format(top) << "\n";
    ++snapshots;
    return true;
  };
  const auto start = std::chrono::steady_clock::now();
  std::string line;
  int number = 0;
  while (std::getline(in, line)) {
    ++number;
    const std::string_view text = command_text(line);
    if (text.empty()) continue;
    std::string error;
    const auto command = parse_command(text, &error);
    if (!command) {
      std::cerr << "line " << number << ": " << error << "\n";
      return 1;
    }
    events.clear();
    updates.clear();
    book.apply(*command, events);
    std::uint64_t sequence = 0;
    try {
      sequence = feed.apply(*command, events, updates);
    } catch (const std::logic_error& disagreement) {
      std::cerr << "line " << number << ": " << disagreement.what() << "\n";
      return 1;
    }
    trades += static_cast<std::uint64_t>(
        std::count_if(events.begin(), events.end(), [](const Event& e) { return std::holds_alternative<Trade>(e); }));
    changes += updates.size();
    if (args.updates) std::cout << format_updates(sequence, updates) << "\n";
    if (sequence % args.every == 0 && !snapshot()) return 1;
  }
  if ((feed.sequence() == 0 || feed.sequence() % args.every != 0) && !snapshot()) return 1;
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::cerr << "replayed " << feed.sequence() << " commands: " << trades << " trades, " << changes
            << " level updates, " << snapshots << " snapshots; " << seconds << " s with parsing and output ("
            << static_cast<double>(feed.sequence()) / seconds << " commands/s)\n";
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
  if (verb == "version") return rest.empty() && !book_named ? version() : usage();
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
  if (verb == "replay") {
    const auto replay_args = parse_replay_args(rest);
    if (!replay_args) return usage();
    if (!replay_args->file) return engine ? replay<Engine>(std::cin, *replay_args) : replay<ReferenceBook>(std::cin, *replay_args);
    std::ifstream in(*replay_args->file);
    if (!in) {
      std::cerr << *replay_args->file << ": can't read it\n";
      return 1;
    }
    return engine ? replay<Engine>(in, *replay_args) : replay<ReferenceBook>(in, *replay_args);
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
