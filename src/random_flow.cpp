#include "orderbook/random_flow.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace orderbook {

RandomFlow::RandomFlow(std::uint64_t seed, FlowOptions options) : state_(seed), options_(options) {
  if (options.price_range < 0 || options.price_range > (kMaxPrice.ticks - kMinPrice.ticks) / 2) {
    throw std::invalid_argument("price_range must be 0 to " + std::to_string((kMaxPrice.ticks - kMinPrice.ticks) / 2) +
                                ", got " + std::to_string(options.price_range));
  }
  if (!valid_quantity(Quantity{options.max_quantity})) {
    throw std::invalid_argument("max_quantity must be 1 to " + std::to_string(kMaxQuantity.lots) + ", got " +
                                std::to_string(options.max_quantity));
  }
  if (options.invalid_per_mille < 0 || options.invalid_per_mille > 1000) {
    throw std::invalid_argument("invalid_per_mille must be 0 to 1000, got " +
                                std::to_string(options.invalid_per_mille));
  }
  // From the first command on, not only once the mid has first moved.
  mid_ = std::clamp(options.start_price, lowest_mid(), highest_mid());
}

// Far enough from both price bounds that every price this stream means to be valid (the mid plus or minus up to
// price_range) is.
std::int64_t RandomFlow::lowest_mid() const { return kMinPrice.ticks + options_.price_range; }
std::int64_t RandomFlow::highest_mid() const { return kMaxPrice.ticks - options_.price_range; }

// SplitMix64 (Steele, Lea and Flood, 2014): a 64-bit state stepped by a constant and mixed. Small, fast, and passes
// BigCrush, which is plenty for test inputs.
std::uint64_t RandomFlow::next_u64() {
  std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

// Without bias: draws below 2^64 mod n are thrown away, so every remainder is equally likely.
std::uint64_t RandomFlow::below(std::uint64_t n) {
  const std::uint64_t threshold = (0 - n) % n;
  for (;;) {
    const std::uint64_t r = next_u64();
    if (r >= threshold) return r % n;
  }
}

std::int64_t RandomFlow::between(std::int64_t lo, std::int64_t hi) {
  const auto span = static_cast<std::uint64_t>(hi - lo) + 1;
  return lo + static_cast<std::int64_t>(below(span));
}

void RandomFlow::step_mid() {
  if (below(4) != 0) return;
  const std::int64_t step = between(-1, 1);
  mid_ = std::clamp(mid_ + step, lowest_mid(), highest_mid());
}

Command RandomFlow::next() {
  step_mid();
  // Below the target, limit orders outnumber cancels (60 to 25 in 100); above it, cancels win (50 to 35). Either
  // way 10 in 100 are IOC and 5 market orders.
  const std::uint64_t limit_share = candidates_.size() < options_.target_resting ? 60 : 35;
  const std::uint64_t pick = below(100);
  if (pick < limit_share) return new_order(OrderType::Limit);
  if (pick < limit_share + 10) return new_order(OrderType::ImmediateOrCancel);
  if (pick < limit_share + 15) return new_order(OrderType::Market);
  if (candidates_.empty()) return new_order(OrderType::Limit);
  return cancel();
}

NewOrder RandomFlow::new_order(OrderType type) {
  const Side side = below(2) == 0 ? Side::Buy : Side::Sell;
  const std::int64_t quantity = between(1, options_.max_quantity);
  const std::int64_t range = options_.price_range;
  std::int64_t price = 0;
  if (type == OrderType::Limit) {
    // Up to `range` ticks behind the mid on the order's own side, or up to a fifth of that through it.
    const std::int64_t behind = between(-range / 5, range);
    price = side == Side::Buy ? mid_ - behind : mid_ + behind;
  } else if (type == OrderType::ImmediateOrCancel) {
    // Up to half the range through the mid, or a fifth of it short.
    const std::int64_t through = between(-range / 5, range / 2);
    price = side == Side::Buy ? mid_ + through : mid_ - through;
  }

  NewOrder order{OrderId{next_id_++}, side, type, Quantity{quantity}, Price{price}};
  if (below(1000) < static_cast<std::uint64_t>(options_.invalid_per_mille)) {
    spoil(order);
  } else {
    last_valid_id_ = order.id.value;
    if (type == OrderType::Limit) candidates_.push_back(order.id);
  }
  return order;
}

// Breaks exactly one rule, so the order is rejected for that reason.
void RandomFlow::spoil(NewOrder& order) {
  const bool market = order.type == OrderType::Market;
  switch (below(6)) {
    case 0:
      order.id = OrderId{last_valid_id_};  // already accepted, or 0 before anything was
      break;
    case 1:
      order.quantity = Quantity{0};
      break;
    case 2:
      order.quantity = Quantity{-between(1, options_.max_quantity)};
      break;
    case 3:
      order.quantity = Quantity{kMaxQuantity.lots + between(1, 1000)};
      break;
    case 4:
      order.price = market ? Price{mid_} : Price{0};
      break;
    default:
      order.price = market ? Price{-1} : Price{kMaxPrice.ticks + between(1, 1000)};
      break;
  }
}

CancelOrder RandomFlow::cancel() {
  const std::size_t i = below(candidates_.size());
  const OrderId id = candidates_[i];
  candidates_[i] = candidates_.back();
  candidates_.pop_back();
  return CancelOrder{id};
}

}  // namespace orderbook
