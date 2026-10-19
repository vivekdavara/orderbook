// orderbook_gateway: the TCP order gateway in front of the engine (include/orderbook/gateway.hpp has the protocol).
//
//   orderbook_gateway [--port N] [--host IPV4]     (127.0.0.1:8107 by default; --port 0 picks a free port)
//
// It prints the address it listens on, runs until SIGINT or SIGTERM, then prints its counts. With nc:
//
//   $ nc 127.0.0.1 8107
//   new 1 sell limit 10 @ 101
//   accepted 1
//   book
//   snapshot 1 bids - asks 101x10

#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <charconv>
#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include "orderbook/gateway.hpp"

namespace {

int usage() {
  std::fprintf(stderr, "usage: orderbook_gateway [--port N] [--host IPV4]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace orderbook;
  GatewayOptions options;
  for (int i = 1; i < argc; i += 2) {
    const std::string_view flag = argv[i];
    if (i + 1 >= argc) return usage();
    const std::string_view value = argv[i + 1];
    if (flag == "--port") {
      std::uint16_t port = 0;
      const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), port);
      if (value.empty() || ec != std::errc{} || ptr != value.data() + value.size()) return usage();
      options.port = port;
    } else if (flag == "--host") {
      options.host = std::string(value);
    } else {
      return usage();
    }
  }

  // SIGINT and SIGTERM are blocked in every thread (the gateway's and the engine's inherit this mask) and waited for
  // on this one, so no handler runs inside the loop. Their dispositions are reset first: a signal that is ignored is
  // discarded, never left pending for sigwait, and a non-interactive shell starts `&` jobs with SIGINT ignored.
  std::signal(SIGINT, SIG_DFL);
  std::signal(SIGTERM, SIG_DFL);
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);

  std::optional<Gateway> gateway;
  try {
    gateway.emplace(options);
  } catch (const std::system_error& error) {
    std::fprintf(stderr, "orderbook_gateway: %s\n", error.what());
    return 1;
  }
  std::printf("orderbook_gateway: listening on %s:%u\n", options.host.c_str(), static_cast<unsigned>(gateway->port()));
  std::fflush(stdout);

  std::exception_ptr failure;
  std::thread loop([&] {
    try {
      gateway->run();
    } catch (...) {
      failure = std::current_exception();
      kill(getpid(), SIGTERM);  // wake the wait below
    }
  });
  int received = 0;
  sigwait(&signals, &received);
  gateway->stop();
  loop.join();
  if (failure) std::rethrow_exception(failure);  // a socket or depth-feed failure: end as the engine would

  const GatewayStats& stats = gateway->stats();
  std::printf("orderbook_gateway: stopped; %" PRIu64 " sessions, %" PRIu64 " commands to the engine, %" PRIu64
              " answered by the gateway, %" PRIu64 " waits for ring room, %" PRIu64 " sessions dropped\n",
              stats.sessions, stats.commands, stats.gateway_answers, stats.ring_full, stats.dropped_sessions);
  return 0;
}
