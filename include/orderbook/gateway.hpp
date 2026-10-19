#pragma once

// The TCP order gateway (DESIGN.md, "The gateway (day 4)"): clients connect over TCP and send one command a line;
// one thread runs every connection with poll(2), maps each client's order ids to the engine's, sends the commands
// to the engine's thread through its ring and turns the events that come back into each client's answers. It is the
// one sending thread `EngineThread` asks for, and it keeps the depth feed (market_data.hpp) from the events.
//
// The protocol, one line each way per message (a line ends with '\n'; a '\r' before it is ignored):
//
//   client -> gateway                          gateway -> client
//   new <id> <buy|sell> <limit|ioc|market>     accepted <id>
//       <quantity> [@ <price>]                 rejected <id> <reason>
//   cancel <id>                                fill <id> <quantity> @ <price>      (each trade the order is in)
//   book [<levels>]                            cancelled <id> <remaining> <requested|unfilled>
//   subscribe                                  snapshot <sequence> bids <levels> asks <levels>   (book, subscribe)
//                                              update <sequence> [<side> <price>x<total>]...     (to subscribers)
//                                              error <message>                     (a line it can't read)
//
// - Ids are the client's own. A session's new orders must have increasing ids (else `rejected <id>
//   id-not-increasing`, from the gateway), and a cancel names one of the session's orders still live (else `rejected
//   <id> unknown-order`). The gateway gives each new order the next engine id, so the engine's ids increase too,
//   and never tells a client another client's id: a trade is a `fill` to each side, under its own id.
// - Answers come in the order of the session's lines, the gateway's own answers (rejections it makes, `book`,
//   `error`) included: those wait until the session's earlier commands are answered. A `fill` for a resting order is
//   sent when another session's order trades with it. A snapshot is the depth feed's after every command answered so
//   far, `book` with no number giving 10 levels a side.
// - Market data: `subscribe` answers with a snapshot of every level, at sequence N, and from then on the session gets
//   an `update` line for every command after N, from any session, with the levels it changed (none for some): a
//   subscriber that applies them in sequence (L2Book) has the book, and a missing number would show as a gap.
// - A session ends when the client closes its side and every answer is written; after a line longer than the limit
//   (not counting its "\r\n"), once the answers before it and its error are written, nothing after it being read; and
//   at once on a socket error or more unwritten answers than the limit. Its live orders are then cancelled (cancel on
//   disconnect). An `error` for a line it can't read doesn't end a session.
// - Back-pressure: when the engine's ring is full the gateway stops handling lines until it has room (it reads on,
//   up to a limit of unhandled input), and it stops reading from a client past that limit, so the client's sends back
//   up into TCP.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "orderbook/book.hpp"
#include "orderbook/command.hpp"
#include "orderbook/engine_thread.hpp"
#include "orderbook/event.hpp"
#include "orderbook/market_data.hpp"

namespace orderbook {

struct GatewayOptions {
  std::string host = "127.0.0.1";  // loopback only, unless asked: the gateway has no authentication
  std::uint16_t port = 8107;       // 0 picks a free port (tests); port() says which
  BookCapacity capacity{.orders = 65'536, .levels = 16'384};
  std::size_t max_line = 256;              // longer lines end the session
  std::size_t max_unread_input = 1 << 20;  // bytes read but not yet handled before it stops reading a client
  std::size_t max_unsent_output = 4 << 20;  // answers a client hasn't read before the session is ended
};

// Counts for the end of a run. Read them once run() has returned.
struct GatewayStats {
  std::uint64_t sessions = 0;        // connections accepted
  std::uint64_t commands = 0;        // commands sent to the engine, cancels on disconnect included
  std::uint64_t gateway_answers = 0;  // rejections, snapshots and errors the gateway answered itself
  std::uint64_t ring_full = 0;       // tries to send that found the engine's ring full (a command may wait several)
  std::uint64_t dropped_sessions = 0;  // ended for a long line, too much unread output or a socket error
};

class Gateway {
 public:
  // Binds and listens (so a client may connect before run() starts) and starts the engine's thread. Throws
  // std::system_error if the address can't be had.
  explicit Gateway(GatewayOptions options = {});
  Gateway(const Gateway&) = delete;
  Gateway& operator=(const Gateway&) = delete;
  ~Gateway();

  // Runs the event loop on the calling thread until stop(). Call it once.
  void run();
  // Ends run() soon, from any thread (or a signal handler: it only stores a flag and writes a byte to a pipe).
  void stop();

  std::uint16_t port() const { return port_; }
  const GatewayStats& stats() const { return stats_; }

 private:
  // An answer the gateway made itself, waiting for the session's earlier commands to be answered: it is written once
  // `after` of them are.
  struct Held {
    std::uint64_t after = 0;
    std::string text;           // the line, or empty for a snapshot, made when it is written
    std::size_t levels = 0;     // a snapshot's levels
    bool subscribe = false;     // after the snapshot, send the session every command's updates
  };
  struct Session {
    int fd = -1;
    std::string in;            // bytes read, not yet handled
    std::string out;           // answers not yet written
    std::size_t out_sent = 0;  // how much of `out` has been written
    std::uint64_t last_client_id = 0;
    std::unordered_map<std::uint64_t, std::uint64_t> live;  // client id -> engine id, of orders in flight or resting
    std::uint64_t sent = 0;    // commands sent to the engine
    std::uint64_t answered = 0;  // of those, the ones whose last event has come back
    std::deque<Held> held;
    bool subscribed = false;   // gets an update line after every command
    bool read_closed = false;  // the client closed its side: end once everything is answered and written
    bool drop = false;         // end it now
  };
  // A live order: whose it is, under which id, and what is left of it.
  struct Route {
    std::uint64_t session = 0;
    std::uint64_t client_id = 0;
    Quantity remaining;
  };
  // A command in the engine's ring, in the order sent; its events come back in the same order.
  struct InFlight {
    std::uint64_t session = 0;  // 0: the gateway's own (a cancel on disconnect)
    std::uint64_t client_id = 0;
    Command command;            // as the engine got it: engine ids
  };

  void accept_clients();
  void read_from(Session& session);
  // Handles the session's complete lines while the engine's ring has room; false if it stopped for want of room.
  bool handle_lines(std::uint64_t id, Session& session);
  enum class Handled { Done, NoRoom, Drop };
  Handled handle_line(std::uint64_t id, Session& session, std::string_view line);
  bool send_to_engine(std::uint64_t session, std::uint64_t client_id, const Command& command);
  void answer_now_or_later(Session& session, Held held);
  void flush_held(Session& session);
  void receive_reports();
  void handle_report(const Report& report);
  void command_answered(const InFlight& done);
  void forget(std::uint64_t engine_id);
  void say(std::uint64_t session, const std::string& line);
  void write_out(Session& session);
  void end_sessions();
  void end_session(Session& session);
  void drop(Session& session);
  void too_long(Session& session);
  bool send_cancels_on_disconnect();

  GatewayOptions options_;
  int listen_fd_ = -1;
  int wake_read_ = -1;
  int wake_write_ = -1;
  std::uint16_t port_ = 0;
  std::atomic<bool> stopping_{false};
  std::unique_ptr<EngineThread> engine_;
  DepthFeed feed_;

  std::unordered_map<std::uint64_t, Session> sessions_;
  std::uint64_t next_session_ = 1;
  std::size_t subscribers_ = 0;
  std::unordered_map<std::uint64_t, Route> routes_;  // engine id -> the order's owner
  std::deque<InFlight> in_flight_;
  std::deque<std::uint64_t> pending_cancels_;  // engine ids to cancel for sessions that ended, when the ring has room
  std::uint64_t next_engine_id_ = 1;
  std::uint64_t next_sequence_ = 1;
  std::vector<Event> events_;  // the events of the command at the front of in_flight_, so far
  std::vector<LevelUpdate> updates_;
  GatewayStats stats_;
};

}  // namespace orderbook
