#pragma once

// The engine on a thread of its own (DESIGN.md, "Threads"): the shape day 4's gateway drives. Commands arrive through
// one SPSC ring and the events go back through another, so the book needs no lock, and the engine's thread waits on
// nothing but an empty ring of requests or a full ring of reports. It spins while it waits (a core of its own, as a
// low-latency engine runs), pausing the core between looks.
//
// One sending thread uses `try_send` and `try_receive`; it must keep receiving while it waits to send, or both
// threads can end up waiting on a full ring each.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

#include "orderbook/book.hpp"
#include "orderbook/command.hpp"
#include "orderbook/event.hpp"
#include "orderbook/spsc_ring.hpp"

namespace orderbook {

// A command with the sender's number for it, which its events carry back.
struct Request {
  std::uint64_t sequence = 0;
  Command command;
};

// One event of a request's answer. Every command has at least one event (DESIGN.md, "Events"); the last is flagged,
// so the sender knows the command is done.
struct Report {
  std::uint64_t sequence = 0;
  Event event;
  bool last = false;
};

// A hint to the core that this thread is spinning: `pause` on x86, `yield` on ARM.
inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield");
#endif
}

class EngineThread {
 public:
  static constexpr std::size_t kRingSize = std::size_t{1} << 14;
  using Requests = SpscRing<Request, kRingSize>;
  using Reports = SpscRing<Report, kRingSize>;

  // Starts the engine's thread on an empty book with the given room.
  explicit EngineThread(BookCapacity capacity = {});
  EngineThread(const EngineThread&) = delete;
  EngineThread& operator=(const EngineThread&) = delete;
  ~EngineThread() { stop(); }

  // The sending thread's side. False if the ring is full (send) or empty (receive).
  bool try_send(const Request& request) { return requests_->try_push(request); }
  bool try_receive(Report& report) { return reports_->try_pop(report); }

  // Lets the engine apply everything already sent, then waits for its thread to end. If the ring of reports fills up
  // meanwhile, the engine stops there instead: the caller has stopped receiving, so it drops the rest. Receive every
  // report you want before stopping. Idempotent.
  void stop();

  // The book. Only once stopped: until then the engine's thread owns it.
  const Book& book() const { return book_; }
  // Commands applied so far, as the engine's thread last counted. A count to watch, not a way to read the book: it
  // orders nothing (relaxed), and the book is only readable once stopped.
  std::uint64_t applied() const { return applied_.load(std::memory_order_relaxed); }

 private:
  void run();

  std::unique_ptr<Requests> requests_ = std::make_unique<Requests>();
  std::unique_ptr<Reports> reports_ = std::make_unique<Reports>();
  Book book_;
  std::atomic<bool> stopping_{false};
  std::atomic<std::uint64_t> applied_{0};
  std::thread thread_;  // last: it starts once everything above exists
};

}  // namespace orderbook
