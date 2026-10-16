#pragma once

// A seeded stream of random commands, for invariant runs, differential tests (day 2) and benchmarks.
//
// The same seed and options give the same commands on every platform and compiler: the generator is SplitMix64
// with its own unbiased bounded draw, not <random>'s distributions, which the standard leaves to each library
// (std::uniform_int_distribution gives different numbers with libstdc++ and libc++). Each draw is its own
// statement, because the order a function's arguments are evaluated in is unspecified too.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "orderbook/command.hpp"
#include "orderbook/types.hpp"

namespace orderbook {

struct FlowOptions {
  std::int64_t start_price = 10'000;  // the mid price the stream starts at, in ticks; it then wanders by a tick
  std::int64_t price_range = 50;      // limit and IOC prices fall within this many ticks of the mid
  std::int64_t max_quantity = 100;    // quantities are uniform in 1..max_quantity
  std::size_t target_resting = 500;   // past this many limit orders that may still rest, cancels outnumber them
  int invalid_per_mille = 5;          // new orders given one invalid field, to exercise every rejection
};

class RandomFlow {
 public:
  explicit RandomFlow(std::uint64_t seed, FlowOptions options = {});

  // The next command. Of new orders: limit orders mostly rest near the mid, some cross it; IOC orders mostly cross
  // it; market orders cross at any price. Cancels pick a limit order this stream sent and hasn't cancelled yet,
  // which may have filled since (and is then rejected).
  Command next();

  // Limit orders sent, valid and not yet cancelled by this stream: an upper bound on what rests from it.
  std::size_t cancel_candidates() const { return candidates_.size(); }

 private:
  std::uint64_t next_u64();
  std::uint64_t below(std::uint64_t n);                  // uniform in [0, n), n > 0
  std::int64_t between(std::int64_t lo, std::int64_t hi);  // uniform in [lo, hi]

  NewOrder new_order(OrderType type);
  void spoil(NewOrder& order);
  CancelOrder cancel();
  void step_mid();

  std::uint64_t state_;
  FlowOptions options_;
  std::int64_t mid_;
  std::uint64_t next_id_ = 1;
  std::uint64_t last_valid_id_ = 0;  // the newest id of an order sent valid, which the book has therefore accepted
  std::vector<OrderId> candidates_;
};

}  // namespace orderbook
