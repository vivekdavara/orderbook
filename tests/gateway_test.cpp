// The gateway over real TCP connections on a free port: a client's orders under its own ids, fills to both sides of
// a trade without either learning the other's id, the client id rules, answers in the order of the lines (the
// gateway's own included), snapshots from the depth feed, cancel on disconnect, a client that closes its side first,
// long lines; and 20,000 random commands from one client answered exactly as the engine answers them directly.

#include "orderbook/gateway.hpp"

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <memory>
#include <system_error>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "orderbook/book.hpp"
#include "orderbook/market_data.hpp"
#include "orderbook/random_flow.hpp"
#include "orderbook/text.hpp"
#include "orderbook/would_block.hpp"

namespace orderbook {
namespace {

using Lines = std::vector<std::string>;

// A gateway on a free port, its loop on a thread of its own.
class RunningGateway {
 public:
  explicit RunningGateway(GatewayOptions options = {.port = 0})
      : gateway_(std::move(options)), thread_([this] { gateway_.run(); }) {}
  RunningGateway(const RunningGateway&) = delete;
  RunningGateway& operator=(const RunningGateway&) = delete;
  ~RunningGateway() {
    gateway_.stop();
    thread_.join();
  }
  std::uint16_t port() const { return gateway_.port(); }

 private:
  Gateway gateway_;
  std::thread thread_;
};

// A blocking client with a 10-second receive timeout, so a missing answer fails the test instead of hanging it. A
// write to a gateway that has closed fails the test rather than killing it with SIGPIPE. `receive_buffer`, if set,
// is the socket's receive buffer, set before connecting (a client that reads slowly).
class Client {
 public:
  explicit Client(std::uint16_t port, int receive_buffer = 0) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (receive_buffer > 0) ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof receive_buffer);
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
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  ~Client() { close(); }

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
  // The next line, or "<eof>" when the gateway has closed the connection, "<timeout>", or "<error>" (a reset).
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
      if (n < 0) return would_block(errno) ? "<timeout>" : "<error>";
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
  // One send, without failing the test: false once the gateway has closed the connection.
  bool try_send(std::string_view text) {
#ifdef MSG_NOSIGNAL
    return ::send(fd_, text.data(), text.size(), MSG_NOSIGNAL) > 0;
#else
    return ::send(fd_, text.data(), text.size(), 0) > 0;
#endif
  }
  void close_write() { ::shutdown(fd_, SHUT_WR); }
  // Closes with a reset (RST) rather than an orderly end: SO_LINGER with a time of 0.
  void reset() {
    linger abort{};
    abort.l_onoff = 1;
    abort.l_linger = 0;
    ::setsockopt(fd_, SOL_SOCKET, SO_LINGER, &abort, sizeof abort);
    close();
  }
  void close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

 private:
  int fd_ = -1;
  std::string buffer_;
};

// Asks for the book until it is `expected` (the gateway's cancels on disconnect take a moment), for up to 5 s.
std::string book_becomes(Client& client, std::string_view expected_levels) {
  std::string last;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    last = client.ask("book\n", 1).front();
    if (last.ends_with(expected_levels)) return last;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return last;
}

TEST(Gateway, ATradeIsAFillToEachSideUnderItsOwnId) {
  RunningGateway gateway;
  Client seller(gateway.port());
  Client buyer(gateway.port());
  EXPECT_EQ(seller.ask("new 7 sell limit 10 @ 101\n", 1), (Lines{"accepted 7"}));
  EXPECT_EQ(buyer.ask("new 3 buy limit 4 @ 101\n", 2), (Lines{"accepted 3", "fill 3 4 @ 101"}));
  EXPECT_EQ(seller.lines(1), (Lines{"fill 7 4 @ 101"}));
  // Both clients may use the same ids: they are each session's own.
  EXPECT_EQ(buyer.ask("new 7 buy limit 6 @ 102\n", 2), (Lines{"accepted 7", "fill 7 6 @ 101"}));
  EXPECT_EQ(seller.lines(1), (Lines{"fill 7 6 @ 101"}));
  EXPECT_EQ(seller.ask("book\n", 1), (Lines{"snapshot 3 bids - asks -"}));
}

TEST(Gateway, CancelsNameTheClientsOwnIds) {
  RunningGateway gateway;
  Client client(gateway.port());
  Client other(gateway.port());
  EXPECT_EQ(client.ask("new 1 sell limit 5 @ 101\n", 1), (Lines{"accepted 1"}));
  EXPECT_EQ(other.ask("cancel 1\n", 1), (Lines{"rejected 1 unknown-order"}));  // not its order 1
  EXPECT_EQ(client.ask("cancel 1\n", 1), (Lines{"cancelled 1 5 requested"}));
  EXPECT_EQ(client.ask("cancel 1\n", 1), (Lines{"rejected 1 unknown-order"}));
  EXPECT_EQ(client.ask("cancel 42\n", 1), (Lines{"rejected 42 unknown-order"}));
  // A filled order is no longer live either.
  EXPECT_EQ(client.ask("new 2 sell limit 5 @ 101\n", 1), (Lines{"accepted 2"}));
  EXPECT_EQ(other.ask("new 1 buy ioc 5 @ 101\n", 2), (Lines{"accepted 1", "fill 1 5 @ 101"}));
  EXPECT_EQ(client.lines(1), (Lines{"fill 2 5 @ 101"}));
  EXPECT_EQ(client.ask("cancel 2\n", 1), (Lines{"rejected 2 unknown-order"}));
}

TEST(Gateway, ASessionsNewOrderIdsMustIncrease) {
  RunningGateway gateway;
  Client client(gateway.port());
  EXPECT_EQ(client.ask("new 5 buy limit 1 @ 100\n", 1), (Lines{"accepted 5"}));
  EXPECT_EQ(client.ask("new 5 buy limit 1 @ 100\n", 1), (Lines{"rejected 5 id-not-increasing"}));
  EXPECT_EQ(client.ask("new 4 buy limit 1 @ 100\n", 1), (Lines{"rejected 4 id-not-increasing"}));
  EXPECT_EQ(client.ask("new 0 buy limit 1 @ 100\n", 1), (Lines{"rejected 0 id-not-increasing"}));
  EXPECT_EQ(client.ask("new 6 buy limit 1 @ 100\n", 1), (Lines{"accepted 6"}));
  // An id the engine rejects for another reason is still used up at the gateway.
  EXPECT_EQ(client.ask("new 9 buy limit 0 @ 100\n", 1), (Lines{"rejected 9 invalid-quantity"}));
  EXPECT_EQ(client.ask("new 9 buy limit 1 @ 100\n", 1), (Lines{"rejected 9 id-not-increasing"}));
}

TEST(Gateway, TheEnginesAnswersComeBackUnderTheClientsIds) {
  RunningGateway gateway;
  Client client(gateway.port());
  EXPECT_EQ(client.ask("new 1 buy limit 5 @ 0\n", 1), (Lines{"rejected 1 invalid-price"}));
  EXPECT_EQ(client.ask("new 2 buy market 5 @ 100\n", 1), (Lines{"rejected 2 invalid-price"}));
  EXPECT_EQ(client.ask("new 3 buy market 5\n", 2), (Lines{"accepted 3", "cancelled 3 5 unfilled"}));
  EXPECT_EQ(client.ask("new 4 sell limit 2 @ 100\n", 1), (Lines{"accepted 4"}));
  EXPECT_EQ(client.ask("new 5 buy ioc 3 @ 100\n", 4),
            (Lines{"accepted 5", "fill 5 2 @ 100", "fill 4 2 @ 100", "cancelled 5 1 unfilled"}));  // a self-trade
}

TEST(Gateway, AnswersComeInTheOrderOfTheLinesTheGatewaysOwnIncluded) {
  RunningGateway gateway;
  Client client(gateway.port());
  // One write, so the gateway reads all of it at once: its own answers (the unknown cancel, the book, the error)
  // must wait for the commands before them.
  client.send("new 1 sell limit 5 @ 101\ncancel 9\nbook\nnew 2 buy ioc 2 @ 101\nbook 1\nsell 3\n");
  const Lines answers = client.lines(8);
  EXPECT_EQ(answers[0], "accepted 1");
  EXPECT_EQ(answers[1], "rejected 9 unknown-order");
  EXPECT_EQ(answers[2], "snapshot 1 bids - asks 101x5");
  EXPECT_EQ(answers[3], "accepted 2");
  EXPECT_EQ(answers[4], "fill 2 2 @ 101");
  EXPECT_EQ(answers[5], "fill 1 2 @ 101");
  EXPECT_EQ(answers[6], "snapshot 2 bids - asks 101x3");
  EXPECT_TRUE(answers[7].starts_with("error ")) << answers[7];
}

TEST(Gateway, BookGivesTheDepthFeedsTopLevels) {
  RunningGateway gateway;
  Client client(gateway.port());
  client.send("new 1 buy limit 1 @ 98\nnew 2 buy limit 2 @ 100\nnew 3 buy limit 3 @ 99\nnew 4 sell limit 4 @ 103\n"
              "new 5 sell limit 5 @ 101\nnew 6 sell limit 6 @ 102\n");
  client.lines(6);
  EXPECT_EQ(client.ask("book 2\n", 1), (Lines{"snapshot 6 bids 100x2 99x3 asks 101x5 102x6"}));
  EXPECT_EQ(client.ask("book\n", 1), (Lines{"snapshot 6 bids 100x2 99x3 98x1 asks 101x5 102x6 103x4"}));
  EXPECT_EQ(client.ask("  book \t 1 \n", 1), (Lines{"snapshot 6 bids 100x2 asks 101x5"}));
  for (const std::string_view bad : {"book 0\n", "book 1001\n", "book x\n", "book 1 2\n", "book -1\n"}) {
    const Lines answer = client.ask(bad, 1);
    EXPECT_TRUE(answer.front().starts_with("error expected: book [levels]")) << bad << answer.front();
  }
  const Lines booking = client.ask("booking\n", 1);
  EXPECT_TRUE(booking.front().starts_with("error ")) << booking.front();
  EXPECT_FALSE(booking.front().starts_with("error expected: book")) << booking.front();
}

TEST(Gateway, ADisconnectCancelsTheSessionsLiveOrders) {
  RunningGateway gateway;
  Client watcher(gateway.port());
  {
    Client client(gateway.port());
    EXPECT_EQ(client.ask("new 1 sell limit 5 @ 101\nnew 2 buy limit 3 @ 99\n", 2), (Lines{"accepted 1", "accepted 2"}));
    EXPECT_EQ(watcher.ask("book\n", 1), (Lines{"snapshot 2 bids 99x3 asks 101x5"}));
  }  // closed
  EXPECT_EQ(book_becomes(watcher, "bids - asks -"), "snapshot 4 bids - asks -");  // its two cancels
}

TEST(Gateway, AClientThatClosesItsSideFirstStillGetsEveryAnswer) {
  RunningGateway gateway;
  Client watcher(gateway.port());
  Client client(gateway.port());
  client.send("new 1 sell limit 5 @ 101\nnew 2 sell limit 1 @ 102\nbook");  // the last line has no '\n'
  client.close_write();
  EXPECT_EQ(client.lines(4), (Lines{"accepted 1", "accepted 2", "snapshot 2 bids - asks 101x5 102x1", "<eof>"}));
  // Once answered, the session ended, and its orders were cancelled.
  EXPECT_EQ(book_becomes(watcher, "bids - asks -"), "snapshot 4 bids - asks -");
}

TEST(Gateway, ALineLongerThanTheLimitEndsTheSession) {
  RunningGateway gateway;
  Client client(gateway.port());
  EXPECT_EQ(client.ask("new 1 sell limit 5 @ 101\n", 1), (Lines{"accepted 1"}));
  client.send(std::string(300, 'x') + "\n");
  EXPECT_EQ(client.lines(2), (Lines{"error line longer than 256 bytes", "<eof>"}));
  // Without its newline too: the gateway doesn't wait for one past the limit.
  Client other(gateway.port());
  other.send(std::string(257, 'y'));
  EXPECT_EQ(other.lines(2), (Lines{"error line longer than 256 bytes", "<eof>"}));
  // A line of exactly the limit is a line, and the session goes on.
  Client third(gateway.port());
  EXPECT_TRUE(third.ask(std::string(256, 'z') + "\n", 1).front().starts_with("error "));
  EXPECT_EQ(book_becomes(third, "bids - asks -"), "snapshot 2 bids - asks -");  // order 1, cancelled on disconnect
}

TEST(Gateway, BlankLinesAndCarriageReturnsAreIgnored) {
  RunningGateway gateway;
  Client client(gateway.port());
  EXPECT_EQ(client.ask("\n  \n\t\nnew 1 buy limit 2 @ 100\r\nbook\r\n", 2),
            (Lines{"accepted 1", "snapshot 1 bids 100x2 asks -"}));
}

// Each order the gateway is done with is forgotten (route and live id): a filled maker, a cancelled order, an IOC's
// remainder, a rejected order, a limit order filled as it arrived. So a disconnect sends exactly one cancel here, for
// the one order still resting; a leaked route would send more (each a command, so the snapshot's sequence shows it).
TEST(Gateway, ADisconnectCancelsOnlyTheOrdersStillLive) {
  RunningGateway gateway;
  Client watcher(gateway.port());
  Client other(gateway.port());
  {
    Client client(gateway.port());
    EXPECT_EQ(client.ask("new 1 sell limit 5 @ 101\n", 1), (Lines{"accepted 1"}));                 // 1
    EXPECT_EQ(other.ask("new 1 buy ioc 5 @ 101\n", 2), (Lines{"accepted 1", "fill 1 5 @ 101"}));  // 2
    EXPECT_EQ(client.lines(1), (Lines{"fill 1 5 @ 101"}));  // a maker filled
    EXPECT_EQ(client.ask("new 2 sell limit 5 @ 102\ncancel 2\n", 2),
              (Lines{"accepted 2", "cancelled 2 5 requested"}));                                         // 3, 4
    EXPECT_EQ(client.ask("new 3 buy limit 5 @ 99\n", 1), (Lines{"accepted 3"}));                     // 5: stays live
    EXPECT_EQ(client.ask("new 4 buy ioc 5 @ 90\n", 2), (Lines{"accepted 4", "cancelled 4 5 unfilled"}));  // 6
    EXPECT_EQ(client.ask("new 5 buy limit 0 @ 99\n", 1), (Lines{"rejected 5 invalid-quantity"}));    // 7
    EXPECT_EQ(other.ask("new 2 sell limit 3 @ 105\n", 1), (Lines{"accepted 2"}));                    // 8
    EXPECT_EQ(client.ask("new 6 buy limit 3 @ 105\n", 2), (Lines{"accepted 6", "fill 6 3 @ 105"}));  // 9
    EXPECT_EQ(other.lines(1), (Lines{"fill 2 3 @ 105"}));
  }  // closed: one cancel, command 10
  EXPECT_EQ(book_becomes(watcher, "bids - asks -"), "snapshot 10 bids - asks -");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // any further cancels would be answered by now
  EXPECT_EQ(watcher.ask("book\n", 1), (Lines{"snapshot 10 bids - asks -"}));
}

TEST(Gateway, AConnectionResetEndsTheSessionAndCancelsItsOrders) {
  RunningGateway gateway;
  Client watcher(gateway.port());
  Client client(gateway.port());
  EXPECT_EQ(client.ask("new 1 sell limit 5 @ 101\n", 1), (Lines{"accepted 1"}));
  client.reset();
  EXPECT_EQ(book_becomes(watcher, "bids - asks -"), "snapshot 2 bids - asks -");
}

TEST(Gateway, MoreLinesThanATurnInOneReadAreAllAnswered) {
  RunningGateway gateway;
  Client client(gateway.port());
  std::string many;
  for (int i = 0; i < 1'000; ++i) many += "book 1\n";  // answered by the gateway itself: nothing in the engine
  const Lines answers = client.ask(many, 1'000);
  ASSERT_EQ(answers.size(), 1'000U);
  EXPECT_EQ(answers.back(), "snapshot 0 bids - asks -");
  // And with the client's side closed after them: the session ends only once all are answered.
  Client closing(gateway.port());
  closing.send(many);
  closing.close_write();
  const Lines all = closing.lines(1'001);
  ASSERT_EQ(all.size(), 1'001U);
  EXPECT_EQ(all[999], "snapshot 0 bids - asks -");
  EXPECT_EQ(all[1'000], "<eof>");
}

// It asks for a book of 1,000 levels (about 7 KB an answer for 10 bytes sent) and never reads the answers. How much
// the kernels buffer on the way differs (on Linux a megabyte of answers was absorbed, on macOS tens of megabytes), so
// it asks 6,400 times (some 45 MB of answers), then sends a little every 10 ms until the connection breaks, for up to
// 10 s.
TEST(Gateway, AClientThatDoesntReadItsAnswersIsDropped) {
  GatewayOptions options{.port = 0};
  options.max_unsent_output = 64 << 10;
  RunningGateway gateway(options);
  Client trader(gateway.port());
  std::string orders;
  for (int i = 1; i <= 1'000; ++i) orders += "new " + std::to_string(i) + " buy limit 1 @ " + std::to_string(1'000 + i) + "\n";
  trader.send(orders);
  ASSERT_EQ(trader.lines(1'000).back(), "accepted 1000");
  Client slow(gateway.port(), /*receive_buffer=*/4'096);
  std::string requests;
  for (int i = 0; i < 6'400; ++i) requests += "book 1000\n";
  bool dropped = !slow.try_send(requests);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!dropped && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    dropped = !slow.try_send("book 1000\n");
  }
  ASSERT_TRUE(dropped) << "still connected after 10 s";
  std::string last;
  while ((last = slow.line()).starts_with("snapshot")) {
  }
  EXPECT_TRUE(last == "<eof>" || last == "<error>") << last;  // dropped, not merely slow
}

// "snapshot 2 bids 99x3 asks 101x5" back into a snapshot ("-" for an empty side).
L2Snapshot parse_snapshot(const std::string& line) {
  L2Snapshot snapshot;
  const std::size_t bids = line.find(" bids ");
  const std::size_t asks = line.find(" asks ");
  EXPECT_TRUE(line.starts_with("snapshot ") && bids != std::string::npos && asks != std::string::npos) << line;
  if (bids == std::string::npos || asks == std::string::npos) return snapshot;
  snapshot.sequence = std::stoull(line.substr(9, bids - 9));
  const auto side = [](std::string text) {
    return text == "-" ? std::vector<Level>{} : parse_levels(text).value_or(std::vector<Level>{});
  };
  snapshot.bids = side(line.substr(bids + 6, asks - bids - 6));
  snapshot.asks = side(line.substr(asks + 6));
  return snapshot;
}

// "update 8 sell 101x0 buy 101x3" back into a sequence number and updates.
std::uint64_t parse_update(const std::string& line, std::vector<LevelUpdate>& updates) {
  updates.clear();
  std::vector<std::string> words;
  for (std::size_t i = 0; i < line.size();) {
    const std::size_t end = std::min(line.find(' ', i), line.size());
    words.push_back(line.substr(i, end - i));
    i = end + 1;
  }
  EXPECT_TRUE(words.size() >= 2 && words[0] == "update" && words.size() % 2 == 0) << line;
  if (words.size() < 2) return 0;
  for (std::size_t i = 2; i + 1 < words.size(); i += 2) {
    const auto level = parse_levels(words[i + 1]);
    EXPECT_TRUE(level.has_value() && level->size() == 1) << line;
    if (!level || level->empty()) continue;
    updates.push_back(LevelUpdate{words[i] == "buy" ? Side::Buy : Side::Sell, level->front().price,
                                  level->front().quantity});
  }
  return std::stoull(words[1]);
}

TEST(Gateway, ASubscriberFollowsTheBookFromItsSnapshotAndAnUpdateForEveryCommand) {
  RunningGateway gateway;
  Client trader(gateway.port());
  EXPECT_EQ(trader.ask("new 1 sell limit 5 @ 101\nnew 2 buy limit 3 @ 99\n", 2), (Lines{"accepted 1", "accepted 2"}));
  Client subscriber(gateway.port());
  const std::string first = subscriber.ask("subscribe\n", 1).front();
  EXPECT_EQ(first, "snapshot 2 bids 99x3 asks 101x5");

  // Another client's random stream, then a book request, answered once every command before it is.
  RandomFlow flow(41, FlowOptions{.target_resting = 200, .invalid_per_mille = 0});
  std::string text;
  for (int i = 0; i < 5'000; ++i) text += format(flow.next()) + "\n";
  text += "book 1\n";
  Client other(gateway.port());
  std::thread sender([&] { other.send(text); });
  std::string line;
  while (!(line = other.line()).starts_with("snapshot")) ASSERT_FALSE(line.starts_with("<")) << line;
  sender.join();

  // The subscriber has an update line for each command that reached the engine, every sequence number from 3 on (the
  // cancels of orders already filled are answered by the gateway and never reach it), then its own book's answer.
  subscriber.send("book 1000\n");
  L2Book book(parse_snapshot(first));
  std::vector<LevelUpdate> updates;
  std::size_t lines = 0;
  while (!(line = subscriber.line()).starts_with("snapshot")) {
    ASSERT_TRUE(line.starts_with("update ")) << line;
    const std::uint64_t sequence = parse_update(line, updates);
    ASSERT_TRUE(book.apply(sequence, updates)) << "a gap: " << line << " after " << book.sequence();
    ++lines;
  }
  EXPECT_EQ(format(book.snapshot(1'000)), line);
  EXPECT_EQ(lines, book.sequence() - 2);
  EXPECT_GT(lines, 4'000U);
  EXPECT_GT(book.depth(Side::Buy).size() + book.depth(Side::Sell).size(), 20U);
  // Subscribing again gives a fresh snapshot, and each update still comes once.
  EXPECT_EQ(subscriber.ask("subscribe\n", 1).front(), line);
  trader.send("new 3 buy limit 1 @ 50\n");
  while (!(line = trader.line()).starts_with("accepted")) ASSERT_TRUE(line.starts_with("fill ")) << line;  // its fills
  EXPECT_EQ(line, "accepted 3");
  const std::string next = std::to_string(book.sequence() + 1);
  const Lines last = subscriber.ask("book 1\n", 2);  // one update line, not two, then the book
  EXPECT_EQ(last[0], "update " + next + " buy 50x1");
  EXPECT_TRUE(last[1].starts_with("snapshot " + next + " ")) << last[1];
}

TEST(Gateway, ASpinningGatewayAnswersTheSame) {
  GatewayOptions options{.port = 0};
  options.spin = true;  // never blocks in poll
  RunningGateway gateway(options);
  Client seller(gateway.port());
  Client buyer(gateway.port());
  EXPECT_EQ(seller.ask("new 7 sell limit 10 @ 101\n", 1), (Lines{"accepted 7"}));
  EXPECT_EQ(buyer.ask("new 3 buy limit 4 @ 101\n", 2), (Lines{"accepted 3", "fill 3 4 @ 101"}));
  EXPECT_EQ(seller.lines(1), (Lines{"fill 7 4 @ 101"}));
  EXPECT_EQ(buyer.ask("book\n", 1), (Lines{"snapshot 2 bids - asks 101x6"}));
}

TEST(Gateway, AnAddressItCantHaveIsAnError) {
  EXPECT_THROW(Gateway(GatewayOptions{.host = "not-an-address", .port = 0}), std::system_error);
  const Gateway first(GatewayOptions{.port = 0});
  EXPECT_THROW(Gateway(GatewayOptions{.port = first.port()}), std::system_error);  // listening already
}

// More commands in one turn than the two rings hold. 100 clients each send 256 IOC orders before the gateway's loop
// starts (it listens from its constructor), so one turn hands the engine 25,600 commands without draining a report.
// Each makes two reports (accepted, then cancelled unfilled on an empty book), so the engine stops once the ring of
// reports holds 8,192 commands' worth, the ring of requests fills behind it with 16,384 more, and the last 1,024 must
// wait for room. None may be lost or answered twice: every client gets every answer, in order.
TEST(Gateway, CommandsThatFindTheRingFullWaitForRoom) {
  constexpr int kClients = 100;
  rlimit limit{};
  ::getrlimit(RLIMIT_NOFILE, &limit);
  if (limit.rlim_cur < 2 * kClients + 50) GTEST_SKIP() << "needs " << 2 * kClients + 50 << " descriptors";
  Gateway gateway(GatewayOptions{.port = 0});
  std::string orders;
  for (int i = 1; i <= 256; ++i) orders += "new " + std::to_string(i) + " buy ioc 1 @ 100\n";
  std::vector<std::unique_ptr<Client>> clients;
  for (int c = 0; c < kClients; ++c) {
    clients.push_back(std::make_unique<Client>(gateway.port()));
    clients.back()->send(orders);
  }
  std::thread loop([&] { gateway.run(); });
  for (const auto& client : clients) {
    const Lines answers = client->lines(512);
    ASSERT_EQ(answers.size(), 512U);
    for (int i = 1; i <= 256; ++i) {
      const auto at = static_cast<std::size_t>(2 * (i - 1));
      ASSERT_EQ(answers[at], "accepted " + std::to_string(i));
      ASSERT_EQ(answers[at + 1], "cancelled " + std::to_string(i) + " 1 unfilled");
    }
  }
  clients.clear();
  gateway.stop();
  loop.join();
  EXPECT_EQ(gateway.stats().commands, 25'600U);
  EXPECT_GT(gateway.stats().ring_full, 0U);  // some did wait for room
}

// The answers a client should get for its commands, from a book applied directly: with one session sending the ids
// RandomFlow makes (1, 2, 3, ... with no spoiled orders), the gateway's engine ids are the client's.
Lines expected_answers(const std::vector<Command>& commands, Book& book) {
  Lines answers;
  std::vector<Event> events;
  for (const Command& command : commands) {
    events.clear();
    book.apply(command, events);
    for (const Event& event : events) {
      if (const auto* trade = std::get_if<Trade>(&event)) {
        const std::string fill = " " + std::to_string(trade->quantity.lots) + " @ " + std::to_string(trade->price.ticks);
        answers.push_back("fill " + std::to_string(trade->taker.value) + fill);
        answers.push_back("fill " + std::to_string(trade->maker.value) + fill);
      } else {
        answers.push_back(format(event));
      }
    }
  }
  return answers;
}

TEST(Gateway, ARandomStreamIsAnsweredAsTheEngineAnswersItDirectly) {
  RandomFlow flow(31, FlowOptions{.target_resting = 300, .invalid_per_mille = 0});
  std::vector<Command> commands;
  std::string text;
  for (int i = 0; i < 20'000; ++i) {
    commands.push_back(flow.next());
    text += format(commands.back()) + "\n";
  }
  Book book;
  const Lines expected = expected_answers(commands, book);

  RunningGateway gateway;
  Client client(gateway.port());
  std::thread sender([&] { client.send(text); });  // all of it at once, read as it comes back
  const Lines answers = client.lines(expected.size());
  sender.join();
  ASSERT_EQ(answers.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    ASSERT_EQ(answers[i], expected[i]) << "answer " << i;
  }
  // And the feed's book is the engine's.
  const std::string book_answer = client.ask("book 1000\n", 1).front();
  const std::string levels = " bids " + format(book.depth(Side::Buy)) + " asks " + format(book.depth(Side::Sell));
  EXPECT_TRUE(book_answer.ends_with(levels)) << book_answer << "\nexpected" << levels;
  EXPECT_GT(book.size(), 150U);
}

}  // namespace
}  // namespace orderbook
