// Review 5 (the day-4 gateway). Each test fails on e88cabe because of one bug; see the comment above it.
//
// Where the order of reads matters, the client connects and sends before the gateway's loop starts (the gateway
// listens from its constructor), so the gateway reads everything in one go and the test is deterministic.

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "orderbook/gateway.hpp"

namespace orderbook {
namespace {

using Lines = std::vector<std::string>;

// A gateway on a free port whose loop starts when start() is called (at once unless asked otherwise).
class TestGateway {
 public:
  explicit TestGateway(bool start_now = true) {
    if (start_now) start();
  }
  TestGateway(const TestGateway&) = delete;
  TestGateway& operator=(const TestGateway&) = delete;
  ~TestGateway() {
    gateway_.stop();
    if (thread_.joinable()) thread_.join();
  }
  void start() {
    thread_ = std::thread([this] { gateway_.run(); });
  }
  std::uint16_t port() const { return gateway_.port(); }

 private:
  Gateway gateway_{GatewayOptions{.port = 0}};
  std::thread thread_;
};

// A blocking client whose receive gives up after `timeout_seconds`, so a missing answer fails the test.
class Client {
 public:
  explicit Client(std::uint16_t port, int timeout_seconds = 5) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = static_cast<std::uint16_t>((port >> 8) | (port << 8));  // network order on these hosts
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    EXPECT_EQ(::connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof address), 0);
    const int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    timeval timeout{};
    timeout.tv_sec = timeout_seconds;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
  }
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  ~Client() {
    if (fd_ >= 0) ::close(fd_);
  }

  void send(std::string_view text) {
    while (!text.empty()) {
      const ssize_t n = ::send(fd_, text.data(), text.size(), 0);
      ASSERT_GT(n, 0) << "send failed";
      text.remove_prefix(static_cast<std::size_t>(n));
    }
  }
  // The next line, or "<eof>" once the gateway has closed the connection, or "<timeout>".
  std::string line() {
    for (;;) {
      const std::size_t end = buffer_.find('\n');
      if (end != std::string::npos) {
        std::string line = buffer_.substr(0, end);
        buffer_.erase(0, end + 1);
        return line;
      }
      char chunk[4096];
      const ssize_t n = ::recv(fd_, chunk, sizeof chunk, 0);
      if (n == 0) return "<eof>";
      if (n < 0) return "<timeout>";
      buffer_.append(chunk, static_cast<std::size_t>(n));
    }
  }
  Lines lines(std::size_t count) {
    Lines out;
    for (std::size_t i = 0; i < count; ++i) {
      out.push_back(line());
      if (out.back() == "<timeout>") break;  // don't wait out the timeout once per missing line
    }
    return out;
  }
  Lines ask(std::string_view text, std::size_t answers) {
    send(text);
    return lines(answers);
  }
  void close_write() { ::shutdown(fd_, SHUT_WR); }

 private:
  int fd_ = -1;
  std::string buffer_;
};

// Bug 1. end_sessions() queues a finished or dropped session's live orders in pending_cancels_, but they are only
// sent by send_cancels_on_disconnect() at the top of the next turn, and the poll timeout just after end_sessions()
// (src/gateway.cpp:171) looks only at in_flight_ and `busy`, both computed before the cancels were queued. With no
// other command in flight the loop blocks in poll(-1), and the departed client's orders stay in the book until some
// other socket wakes the gateway. The existing tests pass only because book_becomes() keeps asking for the book,
// which is what wakes it.
TEST(GatewayReview5, CancelsOnDisconnectGoOutWithoutWaitingForAnotherClient) {
  TestGateway gateway;
  Client watcher(gateway.port());
  Client client(gateway.port());
  EXPECT_EQ(client.ask("new 1 sell limit 5 @ 101\nnew 2 buy limit 3 @ 99\n", 2), (Lines{"accepted 1", "accepted 2"}));
  client.close_write();
  // Its end of file: the gateway has ended the session (end_session closes the socket), so both cancels are due.
  EXPECT_EQ(client.lines(1), (Lines{"<eof>"}));
  std::this_thread::sleep_for(std::chrono::milliseconds(500));  // two cancels take microseconds
  EXPECT_EQ(watcher.ask("book\n", 1), (Lines{"snapshot 4 bids - asks -"}));
}

// Bug 2. A line over the limit puts its error straight into `out` (src/gateway.cpp:238) and drops the session in the
// same turn, so the error is written ahead of the answers to the session's earlier commands still in the engine, and
// those answers are then lost with the session. The header promises that the gateway's own answers, `error`
// included, "wait until the session's earlier commands are answered". Here the client never learns that order 1 was
// accepted (and then cancelled on disconnect).
TEST(GatewayReview5, ALongLinesErrorWaitsForTheAnswersToTheLinesBeforeIt) {
  TestGateway gateway(/*start_now=*/false);
  Client client(gateway.port());
  client.send("new 1 sell limit 5 @ 101\n" + std::string(300, 'x') + "\n");
  gateway.start();
  EXPECT_EQ(client.lines(3), (Lines{"accepted 1", "error line longer than 256 bytes", "<eof>"}));
}

// Bug 3. handle_lines() stops after kLinesPerTurn lines; the too-long check runs only on the lines it reaches, and
// its return value (src/gateway.cpp:254) asks for another turn only if a complete line is left. A partial line
// already past the limit behind 256 lines the gateway answers itself (blank here) is never looked at again: nothing
// is in flight, so the loop sleeps in poll(-1) and the session stays open until the client sends more. The existing
// test says "the gateway doesn't wait for one past the limit".
TEST(GatewayReview5, ALongPartialLineBehindAFullTurnOfLinesStillEndsTheSession) {
  TestGateway gateway(/*start_now=*/false);
  Client client(gateway.port(), /*timeout_seconds=*/2);
  client.send(std::string(256, '\n') + std::string(300, 'x'));
  gateway.start();
  EXPECT_EQ(client.lines(2), (Lines{"error line longer than 256 bytes", "<eof>"}));
}

// Bug 4 (the limit off by one for CRLF clients). The header says a '\r' before the '\n' is ignored, and the existing
// test says "a line of exactly the limit is a line, and the session goes on"; but the length compared with max_line
// (src/gateway.cpp:236-237) still counts the '\r', so the same 256-byte line from a client that ends lines with
// "\r\n" (telnet, nc -C) ends its session.
TEST(GatewayReview5, ALineOfExactlyTheLimitIsALineWhenItEndsInCrLf) {
  TestGateway gateway;
  Client client(gateway.port());
  const Lines answer = client.ask(std::string(256, 'z') + "\r\n", 1);
  ASSERT_EQ(answer.size(), 1U);
  EXPECT_TRUE(answer.front().starts_with("error ")) << answer.front();
  EXPECT_NE(answer.front(), "error line longer than 256 bytes");
  EXPECT_EQ(client.ask("book\n", 1), (Lines{"snapshot 0 bids - asks -"}));
}

}  // namespace
}  // namespace orderbook
