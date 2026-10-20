// Review 6 (17848a7). bench/loadgen.cpp says each row's connections "first build a book of about 1,000 resting
// orders" (lines 12-13 and 224-225; README.md, "Results (day 4)", says the same of the rows it reports). Its warm-up
// is RandomFlow's target_resting = 1,000 / C a connection, but that target counts the flow's cancel candidates
// (orders it sent and hasn't cancelled, filled or not: random_flow.hpp), not what rests; commit 17848a7 made the same
// correction in orderbook_latency, whose 1,000 target rests 583. This test sends loadgen's warm-up (its workload():
// seeds 1000 + c, the target, (target * 4 + 1,000) * C commands, no spoiled orders, round-robin) to a real gateway,
// then counts the book: the connections close, the gateway sends one cancel for each of their resting orders, and
// the feed numbers each command, so the snapshot sequence after the cancels less the one before is the count. On
// 17848a7 it was about 520 on 4 connections and 550 on 1, not about 1,000. Fixed in the docs (the day-4 and day-5
// measurements were made with this warm-up, so it stays): the tests now pin the corrected claim, about 500.

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "orderbook/gateway.hpp"
#include "orderbook/random_flow.hpp"
#include "orderbook/text.hpp"

namespace orderbook {
namespace {

class LoopingGateway {
 public:
  LoopingGateway() : gateway_(GatewayOptions{.port = 0}), thread_([this] { gateway_.run(); }) {}
  LoopingGateway(const LoopingGateway&) = delete;
  LoopingGateway& operator=(const LoopingGateway&) = delete;
  ~LoopingGateway() {
    gateway_.stop();
    thread_.join();
  }
  std::uint16_t port() const { return gateway_.port(); }

 private:
  Gateway gateway_;
  std::thread thread_;
};

// A blocking client with a 10-second receive timeout.
class Connection {
 public:
  explicit Connection(std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
#ifdef SO_NOSIGPIPE
    const int no_sigpipe = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof no_sigpipe);
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = static_cast<std::uint16_t>((port >> 8) | (port << 8));  // network order on these hosts
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    EXPECT_EQ(::connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof address), 0);
    const int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    timeval timeout{};
    timeout.tv_sec = 10;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
  }
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  ~Connection() { close(); }
  void close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

  void send(std::string_view text) {
#ifdef MSG_NOSIGNAL
    constexpr int kFlags = MSG_NOSIGNAL;
#else
    constexpr int kFlags = 0;
#endif
    while (!text.empty()) {
      const ssize_t n = ::send(fd_, text.data(), text.size(), kFlags);
      ASSERT_GT(n, 0) << "send failed";
      text.remove_prefix(static_cast<std::size_t>(n));
    }
  }
  // The next line, or "<eof>" / "<timeout>".
  std::string line() {
    for (;;) {
      if (const std::size_t end = buffer_.find('\n'); end != std::string::npos) {
        std::string line = buffer_.substr(0, end);
        buffer_.erase(0, end + 1);
        return line;
      }
      char chunk[4096];
      const ssize_t n = ::recv(fd_, chunk, sizeof chunk, 0);
      if (n <= 0) return n == 0 ? "<eof>" : "<timeout>";
      buffer_.append(chunk, static_cast<std::size_t>(n));
    }
  }
  // Reads until `count` acknowledgements (loadgen's: accepted, rejected, cancelled ... requested) have come.
  bool acknowledgements(std::size_t count) {
    for (std::size_t seen = 0; seen < count;) {
      const std::string text = line();
      if (text == "<eof>" || text == "<timeout>") return false;
      if (text.starts_with("accepted ") || text.starts_with("rejected ") ||
          (text.starts_with("cancelled ") && text.ends_with(" requested"))) {
        ++seen;
      }
    }
    return true;
  }

 private:
  int fd_ = -1;
  std::string buffer_;
};

// The feed's sequence number in `book`'s answer, and whether the book is empty.
std::uint64_t snapshot_sequence(Connection& watcher, bool& empty) {
  watcher.send("book\n");
  for (;;) {
    const std::string text = watcher.line();
    if (text == "<eof>" || text == "<timeout>") {
      ADD_FAILURE() << "no snapshot";
      return 0;
    }
    if (!text.starts_with("snapshot ")) continue;
    empty = text.ends_with(" bids - asks -");
    return std::stoull(text.substr(9, text.find(' ', 9) - 9));
  }
}

// Orders resting after bench/loadgen.cpp's warm-up on `connections` connections, through a real gateway.
std::size_t resting_after_loadgens_warm_up(std::size_t connections) {
  LoopingGateway gateway;
  // loadgen.cpp's workload(), its warm-up part.
  const std::size_t target = 1'000 / connections;
  std::vector<RandomFlow> flows;
  for (std::size_t c = 0; c < connections; ++c) {
    flows.emplace_back(1'000 + c, FlowOptions{.target_resting = target, .invalid_per_mille = 0});
  }
  std::vector<std::string> lines(connections);
  std::vector<std::size_t> counts(connections, 0);
  for (std::size_t i = 0; i < (target * 4 + 1'000) * connections; ++i) {
    lines[i % connections] += format(flows[i % connections].next()) + "\n";
    ++counts[i % connections];
  }
  std::vector<std::unique_ptr<Connection>> open;
  for (std::size_t c = 0; c < connections; ++c) open.push_back(std::make_unique<Connection>(gateway.port()));
  // Round-robin in rounds of 64 lines a connection, as loadgen keeps up to 1,024 outstanding on each.
  std::vector<std::size_t> at(connections, 0);
  for (bool more = true; more;) {
    more = false;
    for (std::size_t c = 0; c < connections; ++c) {
      std::size_t end = at[c];
      for (int k = 0; k < 64 && end < lines[c].size(); ++k) end = lines[c].find('\n', end) + 1;
      if (end == at[c]) continue;
      open[c]->send(std::string_view(lines[c]).substr(at[c], end - at[c]));
      at[c] = end;
      more = true;
    }
  }
  for (std::size_t c = 0; c < connections; ++c) {
    EXPECT_TRUE(open[c]->acknowledgements(counts[c])) << "connection " << c;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // every command's last event in the feed
  Connection watcher(gateway.port());
  bool empty = false;
  const std::uint64_t before = snapshot_sequence(watcher, empty);
  for (auto& connection : open) connection->close();  // cancel on disconnect: one command a resting order
  std::uint64_t after = before;
  for (int i = 0; i < 500 && !empty; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    after = snapshot_sequence(watcher, empty);
  }
  EXPECT_TRUE(empty) << "the cancels on disconnect didn't empty the book";
  return after - before;  // no cast: uint64_t is size_t on Linux, where GCC calls one useless
}

TEST(LoadgenReview6, FourConnectionsWarmUpToAboutFiveHundredRestingOrders) {
  const std::size_t resting = resting_after_loadgens_warm_up(4);
  EXPECT_GE(resting, 450u) << "loadgen.cpp's \"a book of about 500 resting orders\": " << resting << " rest";
  EXPECT_LE(resting, 600u);
}

TEST(LoadgenReview6, OneConnectionWarmsUpToAboutFiveHundredRestingOrders) {
  const std::size_t resting = resting_after_loadgens_warm_up(1);
  EXPECT_GE(resting, 450u) << "loadgen.cpp's \"a book of about 500 resting orders\": " << resting << " rest";
  EXPECT_LE(resting, 600u);
}

}  // namespace
}  // namespace orderbook
