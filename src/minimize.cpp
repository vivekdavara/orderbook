#include "orderbook/minimize.hpp"

#include <algorithm>

namespace orderbook {

std::vector<Command> minimize(std::vector<Command> commands,
                              const std::function<bool(const std::vector<Command>&)>& fails) {
  if (!fails(commands)) return commands;
  std::size_t chunks = 2;
  std::vector<Command> candidate;
  while (commands.size() >= 2) {
    const std::size_t size = (commands.size() + chunks - 1) / chunks;  // the last chunk may be shorter
    bool removed = false;
    for (std::size_t start = 0; start < commands.size(); start += size) {
      const std::size_t end = std::min(start + size, commands.size());
      candidate.clear();
      candidate.insert(candidate.end(), commands.begin(), commands.begin() + static_cast<std::ptrdiff_t>(start));
      candidate.insert(candidate.end(), commands.begin() + static_cast<std::ptrdiff_t>(end), commands.end());
      if (fails(candidate)) {
        commands.swap(candidate);
        chunks = std::max<std::size_t>(chunks - 1, 2);
        removed = true;
        break;
      }
    }
    if (removed) continue;
    if (chunks >= commands.size()) break;  // no single command can go: 1-minimal
    chunks = std::min(chunks * 2, commands.size());
  }
  // One command left: it is minimal unless the failure doesn't need any command at all.
  if (commands.size() == 1 && fails({})) commands.clear();
  return commands;
}

}  // namespace orderbook
