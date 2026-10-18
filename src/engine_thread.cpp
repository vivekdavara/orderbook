#include "orderbook/engine_thread.hpp"

#include <vector>

namespace orderbook {

EngineThread::EngineThread(BookCapacity capacity) : book_(capacity), thread_([this] { run(); }) {}

void EngineThread::stop() {
  stopping_.store(true, std::memory_order_release);
  if (thread_.joinable()) thread_.join();
}

void EngineThread::run() {
  std::vector<Event> events;
  events.reserve(1'024);  // grows only when a command makes more events than any before it
  Request request;
  for (;;) {
    if (!requests_->try_pop(request)) {
      if (!stopping_.load(std::memory_order_acquire)) {
        cpu_relax();
        continue;
      }
      // The stop was stored after the sender's last push, and the load above saw it, so one more look finds any
      // request sent just before the stop.
      if (!requests_->try_pop(request)) return;
    }
    events.clear();
    book_.apply(request.command, events);
    applied_.store(applied_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    for (std::size_t i = 0; i < events.size(); ++i) {
      const Report report{request.sequence, events[i], i + 1 == events.size()};
      while (!reports_->try_push(report)) {
        if (stopping_.load(std::memory_order_acquire)) return;  // nobody is receiving any more
        cpu_relax();
      }
    }
  }
}

}  // namespace orderbook
