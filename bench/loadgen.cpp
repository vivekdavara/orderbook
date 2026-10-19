// orderbook_loadgen: the TCP gateway's latency and throughput from the clients' side (`make loadgen`).
//
// Clients send RandomFlow commands (seeded per connection, no spoiled orders, so each client's ids increase) over C
// connections, and each command is timed from when it was due to when its acknowledgement arrives: `accepted` or
// `rejected` for a new order, `cancelled <id> <n> requested` or `rejected` for a cancel. A session's answers come in
// the order of its lines (gateway.hpp), so a connection's next acknowledgement is for its oldest command not yet
// acknowledged; the id is checked, and a mismatch stops the run (a gateway bug).
//
// Rows: one at a time on one connection (a command, its acknowledgement, then the next: the round trip with nothing
// queued); paced, command i due at start + i/rate, round-robin over the connections, timed from when it was due, so a
// sender that falls behind counts its wait (no coordinated omission); saturated (rate 0), sending whenever a
// connection has fewer than 1,024 commands outstanding. Each row starts with fresh connections, which first build a
// book of about 1,000 resting orders (untimed); when they close, the gateway cancels what they left.
//
// Usage: orderbook_loadgen [--port N] [--host IPV4] [--connections C] [--commands N] [--rate R]...
// Without --port it starts a gateway in this process, on a free port, on its own thread. Without --rate it runs one
// at a time, then paced at 50,000, 100,000 and 200,000 commands/s, then saturated. Times are in microseconds, from
// std::chrono::steady_clock.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "orderbook/gateway.hpp"
#include "orderbook/random_flow.hpp"
#include "orderbook/text.hpp"
#include "orderbook/would_block.hpp"

namespace orderbook {
namespace {

using Clock = std::chrono::steady_clock;

[[noreturn]] void die(const std::string& message) {
  std::fprintf(stderr, "orderbook_loadgen: %s\n", message.c_str());
  std::exit(1);
}

// One command as a client sends it.
struct Item {
  std::size_t connection = 0;
  std::string line;  // with its '\n'
  std::uint64_t client_id = 0;
  bool cancel = false;
};

struct Pending {
  std::size_t index = 0;
  std::uint64_t client_id = 0;
  bool cancel = false;
};

struct Connection {
  int fd = -1;
  std::string out;
  std::size_t out_sent = 0;
  std::string in;
  std::deque<Pending> pending;  // sent, not yet acknowledged, oldest first
};

int connect_to(const std::string& host, std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) die("socket failed");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = static_cast<std::uint16_t>((port >> 8) | (port << 8));  // network order (little-endian hosts)
  if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) die("not an IPv4 address: " + host);
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
    die("can't connect to " + host + ":" + std::to_string(port));
  }
  const int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  return fd;
}

std::optional<std::uint64_t> id_of(std::string_view word) {
  std::uint64_t id = 0;
  const auto [ptr, ec] = std::from_chars(word.data(), word.data() + word.size(), id);
  if (ec != std::errc{} || ptr != word.data() + word.size()) return std::nullopt;
  return id;
}

// Whether `line` acknowledges a command (and which client id it names): `fill` and an IOC's or market order's
// `cancelled ... unfilled` don't.
std::optional<std::uint64_t> acknowledged(std::string_view line, bool& for_cancel) {
  std::vector<std::string_view> words;
  for (std::size_t i = 0; i < line.size();) {
    const std::size_t end = std::min(line.find(' ', i), line.size());
    words.push_back(line.substr(i, end - i));
    i = end + 1;
  }
  if (words.size() < 2) return std::nullopt;
  if (words[0] == "accepted") {
    for_cancel = false;
    return id_of(words[1]);
  }
  if (words[0] == "rejected") {
    for_cancel = words.size() == 3 && words[2] == "unknown-order";
    return id_of(words[1]);
  }
  if (words[0] == "cancelled" && words.size() == 4 && words[3] == "requested") {
    for_cancel = true;
    return id_of(words[1]);
  }
  if (words[0] == "error") die("the gateway answered: " + std::string(line));
  return std::nullopt;
}

struct Run {
  std::vector<std::int64_t> latency_ns;  // by item
  std::size_t late = 0;                  // paced commands sent more than 50 us after they were due
  double seconds = 0;
};

// Sends the items (round-robin over the connections, as each item says) and waits for every acknowledgement.
// rate > 0: item i is due at start + i / rate. rate 0: as fast as the connections take them, at most `outstanding`
// a connection.
Run drive(std::vector<Connection>& connections, const std::vector<Item>& items, double rate, std::size_t outstanding) {
  Run run;
  run.latency_ns.assign(items.size(), 0);
  std::vector<Clock::time_point> due(items.size());
  const auto start = Clock::now();
  const auto interval = std::chrono::duration<double, std::nano>(rate > 0 ? 1e9 / rate : 0);
  std::size_t next = 0;
  std::size_t acked = 0;
  char chunk[1 << 16];
  while (acked < items.size()) {
    auto now = Clock::now();
    while (next < items.size()) {
      const Item& item = items[next];
      Connection& connection = connections[item.connection];
      if (connection.pending.size() >= outstanding) break;
      const auto when = start + std::chrono::duration_cast<Clock::duration>(interval * static_cast<double>(next));
      if (rate > 0 && now < when) break;
      due[next] = rate > 0 ? when : now;
      if (rate > 0 && now - when > std::chrono::microseconds(50)) ++run.late;
      connection.out += item.line;
      connection.pending.push_back(Pending{next, item.client_id, item.cancel});
      ++next;
    }
    for (Connection& connection : connections) {
      while (connection.out_sent < connection.out.size()) {
        const ssize_t n = ::send(connection.fd, connection.out.data() + connection.out_sent,
                                 connection.out.size() - connection.out_sent, 0);
        if (n <= 0) {
          if (n < 0 && (would_block(errno) || errno == EINTR)) break;
          die("send failed: the gateway closed the connection");
        }
        connection.out_sent += static_cast<std::size_t>(n);
      }
      if (connection.out_sent == connection.out.size()) {
        connection.out.clear();
        connection.out_sent = 0;
      }
      const ssize_t n = ::recv(connection.fd, chunk, sizeof chunk, 0);
      if (n == 0) die("the gateway closed a connection");
      if (n < 0) {
        if (!would_block(errno) && errno != EINTR) die("recv failed");
        continue;
      }
      const auto arrived = Clock::now();
      connection.in.append(chunk, static_cast<std::size_t>(n));
      std::size_t begin = 0;
      for (std::size_t end; (end = connection.in.find('\n', begin)) != std::string::npos; begin = end + 1) {
        const std::string_view line(connection.in.data() + begin, end - begin);
        bool for_cancel = false;
        const auto id = acknowledged(line, for_cancel);
        if (!id) continue;
        if (connection.pending.empty()) die("an acknowledgement for nothing: " + std::string(line));
        const Pending& oldest = connection.pending.front();
        if (*id != oldest.client_id || for_cancel != oldest.cancel) {
          die("expected the acknowledgement of id " + std::to_string(oldest.client_id) + ", got: " +
              std::string(line));
        }
        run.latency_ns[oldest.index] =
            std::chrono::duration_cast<std::chrono::nanoseconds>(arrived - due[oldest.index]).count();
        connection.pending.pop_front();
        ++acked;
      }
      connection.in.erase(0, begin);
    }
  }
  run.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  return run;
}

struct Options {
  std::string host = "127.0.0.1";
  std::optional<std::uint16_t> port;
  std::size_t connections = 4;
  std::size_t commands = 200'000;
  std::vector<double> rates;
};

// A row's workload: connection c's commands come from RandomFlow seed 1000 + c; the warm-up builds about 1,000
// resting orders in all, then the timed commands go round-robin.
struct Workload {
  std::vector<Item> warm_up;
  std::vector<Item> timed;
};

Item item_of(std::size_t connection, const Command& command) {
  Item item;
  item.connection = connection;
  item.line = format(command) + "\n";
  if (const auto* order = std::get_if<NewOrder>(&command)) {
    item.client_id = order->id.value;
  } else {
    item.client_id = std::get<CancelOrder>(command).id.value;
    item.cancel = true;
  }
  return item;
}

Workload workload(std::size_t connections, std::size_t commands) {
  const std::size_t target = 1'000 / connections;
  std::vector<RandomFlow> flows;
  for (std::size_t c = 0; c < connections; ++c) {
    flows.emplace_back(1'000 + c, FlowOptions{.target_resting = target, .invalid_per_mille = 0});
  }
  Workload load;
  for (std::size_t i = 0; i < (target * 4 + 1'000) * connections; ++i) {
    load.warm_up.push_back(item_of(i % connections, flows[i % connections].next()));
  }
  for (std::size_t i = 0; i < commands; ++i) load.timed.push_back(item_of(i % connections, flows[i % connections].next()));
  return load;
}

void print_header() {
  std::printf("%-32s %5s %7s %7s %7s %7s %8s %8s %8s\n", "mode", "conns", "p50", "p90", "p99", "p99.9", "p99.99",
              "max", "mean");
}

void print_row(const std::string& name, std::size_t connections, std::vector<std::int64_t> samples) {
  std::sort(samples.begin(), samples.end());
  const auto at = [&](double q) {  // nearest rank, in microseconds
    const auto rank = static_cast<std::size_t>(q * static_cast<double>(samples.size()) + 0.999999);
    return static_cast<double>(samples[std::min(samples.size(), std::max<std::size_t>(rank, 1)) - 1]) / 1e3;
  };
  double sum = 0;
  for (const std::int64_t s : samples) sum += static_cast<double>(s);
  std::printf("%-32s %5zu %7.1f %7.1f %7.1f %7.1f %8.1f %8.1f %8.1f\n", name.c_str(), connections, at(0.50),
              at(0.90), at(0.99), at(0.999), at(0.9999), static_cast<double>(samples.back()) / 1e3,
              sum / static_cast<double>(samples.size()) / 1e3);
}

// One row: fresh connections, the warm-up, the timed commands, then the connections close.
void row(const Options& options, std::uint16_t port, std::size_t connections, double rate, bool one_at_a_time) {
  const Workload load = workload(connections, options.commands);
  std::vector<Connection> open(connections);
  for (Connection& connection : open) connection.fd = connect_to(options.host, port);
  drive(open, load.warm_up, 0, 1'024);
  const Run run = drive(open, load.timed, one_at_a_time ? 0 : rate, one_at_a_time ? 1 : 1'024);
  for (Connection& connection : open) ::close(connection.fd);
  const double per_second = static_cast<double>(load.timed.size()) / run.seconds;
  char name[64];
  if (one_at_a_time) {
    std::snprintf(name, sizeof name, "one at a time (%.0fk/s)", per_second / 1e3);
  } else if (rate > 0) {
    std::snprintf(name, sizeof name, "paced %.0fk/s (got %.0fk/s)", rate / 1e3, per_second / 1e3);
  } else {
    std::snprintf(name, sizeof name, "saturated (%.0fk/s)", per_second / 1e3);
  }
  print_row(name, connections, run.latency_ns);
  if (rate > 0) std::printf("%32s sent over 50 us late: %zu of %zu\n", "", run.late, load.timed.size());
  std::fflush(stdout);
  // The gateway cancels what these connections left; let it before the next row builds its book.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

int usage() {
  std::fprintf(stderr,
               "usage: orderbook_loadgen [--port N] [--host IPV4] [--connections C] [--commands N] [--rate R]...\n");
  return 2;
}

}  // namespace
}  // namespace orderbook

int main(int argc, char** argv) {
  using namespace orderbook;
  Options options;
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 >= argc) return usage();
    const std::string_view flag = argv[i];
    const std::string value = argv[i + 1];
    char* end = nullptr;
    const double number = std::strtod(value.c_str(), &end);
    const bool numeric = end != value.c_str() && *end == '\0' && number >= 0;
    if (flag == "--host") {
      options.host = value;
    } else if (flag == "--port" && numeric && number >= 1 && number <= 65'535) {
      options.port = static_cast<std::uint16_t>(number);
    } else if (flag == "--connections" && numeric && number >= 1 && number <= 1'000) {
      options.connections = static_cast<std::size_t>(number);
    } else if (flag == "--commands" && numeric && number >= 1) {
      options.commands = static_cast<std::size_t>(number);
    } else if (flag == "--rate" && numeric) {
      options.rates.push_back(number);
    } else {
      return usage();
    }
  }

  // Without --port, a gateway in this process: its loop on a thread of its own (and the engine's on another).
  std::unique_ptr<Gateway> local;
  std::thread loop;
  std::uint16_t port = 0;
  if (options.port) {
    port = *options.port;
  } else {
    local = std::make_unique<Gateway>(GatewayOptions{.port = 0});
    port = local->port();
    loop = std::thread([&] { local->run(); });
  }
  std::printf("orderbook_loadgen: %zu timed commands a row, RandomFlow seeds 1000+ (synthetic), gateway %s:%u%s\n",
              options.commands, options.host.c_str(), static_cast<unsigned>(port),
              local ? " (in this process)" : "");
  std::printf("From when each command was due to its acknowledgement (us):\n");
  print_header();
  if (options.rates.empty()) {
    row(options, port, 1, 0, true);
    for (const double rate : {50e3, 100e3, 200e3}) row(options, port, options.connections, rate, false);
    row(options, port, options.connections, 0, false);
  } else {
    for (const double rate : options.rates) row(options, port, options.connections, rate, false);
  }
  if (local) {
    local->stop();
    loop.join();
    const GatewayStats& stats = local->stats();
    std::printf("gateway: %" PRIu64 " sessions, %" PRIu64 " commands to the engine, %" PRIu64
                " answered by the gateway, %" PRIu64 " waits for ring room, %" PRIu64 " sessions dropped\n",
                stats.sessions, stats.commands, stats.gateway_answers, stats.ring_full, stats.dropped_sessions);
  }
  return 0;
}
