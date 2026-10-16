// A main() for the fuzz targets where libFuzzer isn't available (Apple Clang has no -fsanitize=fuzzer): runs the
// target on each file named on the command line, on each line of each file with `--lines FILE...` (so a target that
// parses one line at a time sees whole commands), or on COUNT random inputs with `--random COUNT SEED`. CTest uses it
// to run both targets on every build, so they can't rot between CI's fuzzing runs.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

int main(int argc, char** argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  if (args.size() == 3 && args[0] == "--random") {
    const unsigned long count = std::stoul(args[1]);
    std::uint64_t state = std::stoull(args[2]);
    std::vector<std::uint8_t> input;
    for (unsigned long i = 0; i < count; ++i) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;  // a 64-bit LCG; quality hardly matters
      input.resize((state >> 33) % 1024);
      for (std::uint8_t& byte : input) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        byte = static_cast<std::uint8_t>(state >> 56);
      }
      LLVMFuzzerTestOneInput(input.data(), input.size());
    }
    std::cout << "ran " << count << " random inputs\n";
    return 0;
  }
  const bool by_line = !args.empty() && args[0] == "--lines";
  std::size_t inputs = 0;
  for (std::size_t i = by_line ? 1 : 0; i < args.size(); ++i) {
    const std::string& path = args[i];
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      std::cerr << path << ": can't read it\n";
      return 1;
    }
    std::ostringstream text;
    text << in.rdbuf();
    const std::string bytes = text.str();
    if (!by_line) {
      LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
      ++inputs;
      continue;
    }
    std::istringstream lines(bytes);
    for (std::string line; std::getline(lines, line); ++inputs) {
      // A command line without its "> " and a depth line without its "= bids " or "= asks ", as parse_command and
      // parse_levels take them; other lines as they are.
      std::string input = line;
      if (line.rfind("> ", 0) == 0) input = line.substr(2);
      if (line.rfind("= bids ", 0) == 0 || line.rfind("= asks ", 0) == 0) input = line.substr(7);
      LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
    }
  }
  std::cout << "ran " << inputs << " inputs\n";
  return 0;
}
