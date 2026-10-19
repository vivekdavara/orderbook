// The gateway over real TCP connections on a free port: a client's orders under its own ids, fills to both sides of
// a trade without either learning the other's id, the client id rules, answers in the order of the lines (the
// gateway's own included), snapshots from the depth feed, cancel on disconnect, a client that closes its side first,
// long lines; and 20,000 random commands from one client answered exactly as the engine answers them directly.

#include "orderbook/gateway.hpp"

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
#include <variant>
#include <vector>

#include "orderbook/book.hpp"
#include "orderbook/random_flow.hpp"
#include "orderbook/text.hpp"

namespace orderbook {
namespace {

using Lines = std::vector<std::string>;

// A gateway on a free port, its loop on a thread of its own.
class RunningGateway {
 public:
  RunningGateway() : thread_([this] { gateway_.run(); }) {}
  RunningGateway(const RunningGateway&) = delete;
  RunningGateway& operator=(const RunningGateway&) = delete;
  ~RunningGateway() {
    gateway_.stop();
    thread_.join();
  }
  std::uint16_t port() const { return gateway_.port(); }

 private:
  Gateway gateway_{GatewayOptions{.port = 0}};
  std::thread thread_;
};

// A blocking client with a 10-second receive timeout, so a missing answer fails the test instead of hanging it.
class Client {
 public:
  explicit Client(std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
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
    while (!text.empty()) {
      const ssize_t n = ::send(fd_, text.data(), text.size(), 0);
      ASSERT_GT(n, 0) << "send failed";
      text.remove_prefix(static_cast<std::size_t>(n));
    }
  }
  // The next line, or "<eof>" when the gateway has closed the connection, or "<timeout>".
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
    for (std::size_t i = 0; i < count; ++i) out.push_back(line());
    return out;
  }
  Lines ask(std::string_view text, std::size_t answers) {
    send(text);
    return lines(answers);
  }
  void close_write() { ::shutdown(fd_, SHUT_WR); }
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
