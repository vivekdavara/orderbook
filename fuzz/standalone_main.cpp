// A main() for the fuzz targets where libFuzzer isn't available (Apple Clang has no -fsanitize=fuzzer): runs the
// target on each file named on the command line, or on COUNT random inputs with `--random COUNT SEED`. CTest uses it
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
  for (const std::string& path : args) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      std::cerr << path << ": can't read it\n";
      return 1;
    }
    std::ostringstream text;
    text << in.rdbuf();
    const std::string bytes = text.str();
    LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
  }
  std::cout << "ran " << args.size() << " files\n";
  return 0;
}
