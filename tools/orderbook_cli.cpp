// orderbook: a command-line tool over the test kit, using the reference model (day 2 adds the engine).
//
//   orderbook check FILE...           run scenario files; exit 1 if any doesn't match
//   orderbook run [FILE]              apply commands (from FILE or stdin) and print a scenario transcript
//   orderbook gen SEED COUNT [opts]   print COUNT random commands, one "> ..." line each
//   orderbook audit SEED COUNT [opts] run a random stream with every invariant check on, and print a summary
//
// gen's and audit's options: --target N, --range N, --max-quantity N, --invalid PER_MILLE (RandomFlow's options).
// `orderbook gen 1 20 | orderbook run` prints what the reference model does with a stream; the transcript is
// itself a scenario file, so `orderbook check` passes it.

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include "orderbook/check.hpp"
#include "orderbook/random_flow.hpp"
#include "orderbook/reference_book.hpp"
#include "orderbook/scenario.hpp"
#include "orderbook/text.hpp"

namespace {

using namespace orderbook;

constexpr int kUsageError = 2;

int usage() {
  std::cerr << "usage: orderbook check FILE...\n"
               "       orderbook run [FILE]\n"
               "       orderbook gen SEED COUNT [--target N] [--range N] [--max-quantity N] [--invalid PER_MILLE]\n"
               "       orderbook audit SEED COUNT [same options as gen]\n";
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
    ReferenceBook book;
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

void print_depth(const ReferenceBook& book) {
  for (const Side side : {Side::Buy, Side::Sell}) {
    const std::string levels = format(book.depth(side));
    std::cout << (side == Side::Buy ? "= bids" : "= asks") << (levels.empty() ? "" : " ") << levels << "\n";
  }
}

int run(std::istream& in) {
  ReferenceBook book;
  std::vector<Event> events;
  std::string line;
  int number = 0;
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
    std::cout << "> " << format(*command) << "\n";
    for (const Event& event : events) std::cout << format(event) << "\n";
  }
  print_depth(book);
  return 0;
}

struct FlowArgs {
  std::uint64_t seed = 0;
  std::uint64_t count = 0;
  FlowOptions options;
};

std::optional<FlowArgs> parse_flow_args(const std::vector<std::string>& args) {
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

int audit(const FlowArgs& args) {
  RandomFlow flow(args.seed, args.options);
  ReferenceBook book;
  Audit checker;
  std::vector<Event> events;
  std::array<std::uint64_t, 4> kinds{};  // accepted or rejected, trades, cancelled; indexed by the variant
  std::uint64_t largest = 0;
  for (std::uint64_t i = 1; i <= args.count; ++i) {
    const Command command = flow.next();
    events.clear();
    book.apply(command, events);
    checker.record(command, events);
    for (const Event& event : events) ++kinds[event.index()];
    if (i % 1000 == 0 || i == args.count) {
      for (const std::string& problem : check_book(book)) std::cerr << "command " << i << ": " << problem << "\n";
      checker.compare(book);
    }
    largest = std::max<std::uint64_t>(largest, book.size());
    if (!checker.ok()) break;
  }
  for (const std::string& problem : checker.problems()) std::cerr << problem << "\n";
  std::cout << "commands " << args.count << ", accepted " << kinds[0] << ", rejected " << kinds[1] << ", trades "
            << kinds[2] << ", cancelled " << kinds[3] << "; resting at the end " << book.size() << ", at most "
            << largest << "\n";
  return checker.ok() && check_book(book).empty() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) return usage();
  const std::string& verb = args[0];
  const std::vector<std::string> rest(args.begin() + 1, args.end());
  if (verb == "check") return check(rest);
  if (verb == "run") {
    if (rest.empty()) return run(std::cin);
    if (rest.size() > 1) return usage();
    std::ifstream in(rest[0]);
    if (!in) {
      std::cerr << rest[0] << ": can't read it\n";
      return 1;
    }
    return run(in);
  }
  if (verb == "gen" || verb == "audit") {
    const auto flow_args = parse_flow_args(rest);
    if (!flow_args) return usage();
    return verb == "gen" ? gen(*flow_args) : audit(*flow_args);
  }
  return usage();
}
