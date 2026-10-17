// Fuzz target for the books: bytes decode to a stream of commands on 16 prices with small quantities (so orders meet
// often), with some invalid orders and cancels of earlier ids. The engine and the reference model run them in
// lockstep: after every command their events and their states must be the same, and the engine's events must pass
// the Audit and its state check_book. Any difference or problem, or a crash, is a finding.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "orderbook/book.hpp"
#include "orderbook/check.hpp"
#include "orderbook/differential.hpp"
#include "orderbook/reference_book.hpp"

namespace {

using namespace orderbook;

class Bytes {
 public:
  Bytes(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
  bool empty() const { return pos_ >= size_; }
  std::uint8_t next() { return pos_ < size_ ? data_[pos_++] : 0; }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
};

// Three bytes a command: what (and which side), a price or the id to cancel, a quantity.
Command decode(Bytes& in, std::uint64_t& next_id) {
  const std::uint8_t what = in.next();
  const std::uint8_t a = in.next();
  const std::uint8_t b = in.next();
  const int kind = what % 8;  // 0-3 limit, 4 ioc, 5 market, 6-7 cancel
  if (kind >= 6) return CancelOrder{OrderId{1 + (std::uint64_t{a} * 256 + b) % next_id}};

  NewOrder order;
  order.id = OrderId{next_id++};
  order.side = (what >> 3) % 2 == 0 ? Side::Buy : Side::Sell;
  order.type = kind == 5 ? OrderType::Market : kind == 4 ? OrderType::ImmediateOrCancel : OrderType::Limit;
  order.quantity = Quantity{b == 255 ? kMaxQuantity.lots : 1 + b % 32};
  order.price = order.type == OrderType::Market ? Price{0} : Price{100 + a % 16};
  if (what >= 0xF0) {  // one in sixteen breaks a rule
    switch (b % 4) {
      case 0:
        order.id = OrderId{a % 2 == 0 ? 0 : order.id.value - 1};  // 0, or the previous id (perhaps accepted)
        break;
      case 1:
        order.quantity = Quantity{0};
        break;
      case 2:
        order.quantity = Quantity{kMaxQuantity.lots + 1};
        break;
      default:
        order.price = order.type == OrderType::Market ? Price{100} : Price{0};
        break;
    }
  }
  return order;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  Bytes in(data, size);
  Lockstep<ReferenceBook, Book> books;
  Audit audit;
  std::uint64_t next_id = 1;
  while (!in.empty()) {
    const Command command = decode(in, next_id);
    if (books.apply(command) || books.compare()) __builtin_trap();
    audit.record(command, books.actual_events());
    if (!audit.ok() || !check_book(books.actual()).empty()) __builtin_trap();
  }
  audit.compare(books.actual());
  if (!audit.ok()) __builtin_trap();
  return 0;
}
