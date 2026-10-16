// Fuzz target for the text format: any bytes, as a command line, a depth line and a whole scenario file. A line that
// parses must format to a line that parses back to the same thing; a scenario file that parses must run on the
// reference model. Nothing may crash (the fuzz build runs under ASan and UBSan).

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "orderbook/reference_book.hpp"
#include "orderbook/scenario.hpp"
#include "orderbook/text.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace orderbook;
  const std::string_view text(reinterpret_cast<const char*>(data), size);

  if (const auto command = parse_command(text)) {
    const auto again = parse_command(format(*command));
    if (!again || *again != *command) __builtin_trap();
  }
  if (const auto levels = parse_levels(text)) {
    const auto again = parse_levels(format(*levels));
    if (!again || *again != *levels) __builtin_trap();
  }
  if (const auto scenario = parse_scenario(text)) {
    ReferenceBook book;
    static_cast<void>(run_scenario(*scenario, book));
  }
  return 0;
}
