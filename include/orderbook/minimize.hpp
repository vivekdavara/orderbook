#pragma once

// Shrinks a failing command stream to a short one that still fails, so that a random run that breaks something is
// reported as a script a person can read (`orderbook run` turns it into a scenario file) instead of thousands of
// commands. Day 2's differential tests use it when the engine and the reference model disagree.
//
// The algorithm is ddmin (Zeller and Hildebrandt, "Simplifying and Isolating Failure-Inducing Input", 2002), in its
// complement form: split the stream into n chunks; if it still fails with one chunk removed, keep the removal and
// split coarser; if no removal keeps it failing, split finer, down to single commands. The result is 1-minimal:
// removing any one command makes the failure go away.

#include <cstddef>
#include <functional>
#include <vector>

#include "orderbook/command.hpp"

namespace orderbook {

// `fails` must replay the commands from scratch (a fresh book each time) and say whether the failure still shows.
// It is called O(n log n) times when chunks fall away easily and O(n^2) at worst. Returns the shrunk stream; if
// `commands` doesn't fail in the first place, returns it unchanged.
std::vector<Command> minimize(std::vector<Command> commands,
                              const std::function<bool(const std::vector<Command>&)>& fails);

}  // namespace orderbook
