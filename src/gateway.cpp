#include "orderbook/gateway.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <charconv>
#include <optional>
#include <system_error>
#include <utility>
#include <variant>

#include "orderbook/text.hpp"

namespace orderbook {
namespace {

// Lines a session may have handled in one turn of the loop, so one busy client can't keep the others waiting.
constexpr int kLinesPerTurn = 256;
constexpr std::size_t kDefaultLevels = 10;
constexpr std::size_t kMaxLevels = 1'000;

[[noreturn]] void fail(const char* what) { throw std::system_error(errno, std::generic_category(), what); }

void set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) fail("fcntl");
}

// Each answer goes out at once rather than waiting to be batched (Nagle's algorithm), and a write to a client that
// has gone fails with EPIPE instead of raising SIGPIPE: SO_NOSIGPIPE on macOS, MSG_NOSIGNAL on each send on Linux.
void set_client_options(int fd) {
  const int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// Network byte order is big-endian (htons is a macro full of casts the warnings flag).
std::uint16_t network_order(std::uint16_t value) {
  if constexpr (std::endian::native == std::endian::little) {
    return static_cast<std::uint16_t>((value >> 8) | (value << 8));
  } else {
    return value;
  }
}

template <class... Fs>
struct Overloaded : Fs... {
  using Fs::operator()...;
};
template <class... Fs>
Overloaded(Fs...) -> Overloaded<Fs...>;

// "book" or "book <levels>": the levels asked for, or an error message. Nullopt if the line isn't a book request.
struct BookRequest {
  std::size_t levels = kDefaultLevels;
  std::string error;
};
std::optional<BookRequest> parse_book(std::string_view line) {
  const auto skip = [&](std::size_t i) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return i;
  };
  const std::size_t start = skip(0);
  if (line.substr(start, 4) != "book") return std::nullopt;
  std::size_t i = start + 4;
  if (i < line.size() && line[i] != ' ' && line[i] != '\t') return std::nullopt;  // "booking"
  i = skip(i);
  BookRequest request;
  if (i == line.size()) return request;
  std::size_t end = i;
  while (end < line.size() && line[end] != ' ' && line[end] != '\t') ++end;
  std::size_t levels = 0;
  const auto [ptr, ec] = std::from_chars(line.data() + i, line.data() + end, levels);
  if (ec != std::errc{} || ptr != line.data() + end || levels == 0 || levels > kMaxLevels || skip(end) != line.size()) {
    request.error = "expected: book [levels], levels 1 to " + std::to_string(kMaxLevels);
  }
  request.levels = levels;
  return request;
}

}  // namespace

Gateway::Gateway(GatewayOptions options) : options_(std::move(options)) {
  try {
    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) fail("pipe");
    wake_read_ = pipe_fds[0];
    wake_write_ = pipe_fds[1];
    set_nonblocking(wake_read_);
    set_nonblocking(wake_write_);

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) fail("socket");
    const int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = network_order(options_.port);
    if (::inet_pton(AF_INET, options_.host.c_str(), &address.sin_addr) != 1) {
      throw std::system_error(EINVAL, std::generic_category(), "not an IPv4 address: " + options_.host);
    }
    if (::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) fail("bind");
    if (::listen(listen_fd_, 128) != 0) fail("listen");
    set_nonblocking(listen_fd_);
    socklen_t length = sizeof address;
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) != 0) fail("getsockname");
    port_ = network_order(address.sin_port);
    engine_ = std::make_unique<EngineThread>(options_.capacity);
  } catch (...) {
    for (const int fd : {listen_fd_, wake_read_, wake_write_}) {
      if (fd >= 0) ::close(fd);
    }
    throw;
  }
}

Gateway::~Gateway() {
  engine_->stop();
  for (auto& [id, session] : sessions_) ::close(session.fd);
  ::close(listen_fd_);
  ::close(wake_read_);
  ::close(wake_write_);
}

void Gateway::stop() {
  stopping_.store(true, std::memory_order_release);
  const char byte = 0;
  [[maybe_unused]] const auto written = ::write(wake_write_, &byte, 1);  // wakes poll(); a full pipe is awake already
}

void Gateway::run() {
  std::vector<pollfd> fds;
  std::vector<std::uint64_t> ids;  // the session of each entry of fds after the first two
  while (!stopping_.load(std::memory_order_acquire)) {
    receive_reports();
    bool busy = !send_cancels_on_disconnect();
    for (auto& [id, session] : sessions_) {
      if (!session.drop && !handle_lines(id, session)) busy = true;
    }
    for (auto& [id, session] : sessions_) write_out(session);
    end_sessions();

    fds.clear();
    ids.clear();
    fds.push_back(pollfd{listen_fd_, POLLIN, 0});
    fds.push_back(pollfd{wake_read_, POLLIN, 0});
    for (const auto& [id, session] : sessions_) {
      short events = 0;
      if (!session.read_closed && session.in.size() < options_.max_unread_input) events = POLLIN;
      if (session.out_sent < session.out.size()) events = static_cast<short>(events | POLLOUT);
      fds.push_back(pollfd{session.fd, events, 0});
      ids.push_back(id);
    }
    // With commands in the engine, look at the sockets without waiting and come back for the reports; otherwise
    // nothing can happen until a socket (or stop()) wakes the loop.
    const int timeout = in_flight_.empty() && !busy ? -1 : 0;
    const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout);
    if (ready < 0) {
      if (errno == EINTR) continue;
      fail("poll");
    }
    if (ready == 0) continue;
    if ((fds[1].revents & POLLIN) != 0) {
      char drain[64];
      while (::read(wake_read_, drain, sizeof drain) > 0) {
      }
    }
    if ((fds[0].revents & POLLIN) != 0) accept_clients();
    for (std::size_t i = 2; i < fds.size(); ++i) {
      const short revents = fds[i].revents;
      if (revents == 0) continue;
      const auto it = sessions_.find(ids[i - 2]);
      if (it == sessions_.end()) continue;
      if ((revents & (POLLIN | POLLHUP | POLLERR)) != 0) read_from(it->second);
      if ((revents & POLLOUT) != 0) write_out(it->second);
    }
  }
}

void Gateway::accept_clients() {
  for (;;) {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EINTR) continue;
      return;  // none left (EAGAIN), or out of descriptors: the next turn tries again
    }
    set_nonblocking(fd);
    set_client_options(fd);
    Session session;
    session.fd = fd;
    sessions_.emplace(next_session_++, std::move(session));
    ++stats_.sessions;
  }
}

void Gateway::read_from(Session& session) {
  char buffer[1 << 16];
  while (!session.read_closed && session.in.size() < options_.max_unread_input) {
    const ssize_t n = ::recv(session.fd, buffer, sizeof buffer, 0);
    if (n > 0) {
      session.in.append(buffer, static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0) {
      // The client closed its side. A last line without its '\n' is still a line.
      session.read_closed = true;
      if (!session.in.empty() && session.in.back() != '\n') session.in.push_back('\n');
      return;
    }
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) drop(session);  // reset by the peer, or another socket error
    return;
  }
}

bool Gateway::handle_lines(std::uint64_t id, Session& session) {
  std::size_t start = 0;
  bool room = true;
  for (int handled = 0; handled < kLinesPerTurn; ++handled) {
    const std::size_t end = session.in.find('\n', start);
    const std::size_t length = (end == std::string::npos ? session.in.size() : end) - start;
    if (length > options_.max_line) {
      session.out += "error line longer than " + std::to_string(options_.max_line) + " bytes\n";
      drop(session);
      start = session.in.size();
      break;
    }
    if (end == std::string::npos) break;
    std::string_view line(session.in.data() + start, length);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (handle_line(id, session, line) == Handled::NoRoom) {
      room = false;
      break;
    }
    start = end + 1;
  }
  session.in.erase(0, start);
  // Lines left after a full turn: come back to them without waiting.
  return room && session.in.find('\n') == std::string::npos;
}

Gateway::Handled Gateway::handle_line(std::uint64_t id, Session& session, std::string_view line) {
  if (line.find_first_not_of(" \t") == std::string_view::npos) return Handled::Done;  // a blank line
  if (const auto book = parse_book(line)) {
    answer_now_or_later(session, book->error.empty() ? Held{session.sent, "", book->levels}
                                                     : Held{session.sent, "error " + book->error, 0});
    return Handled::Done;
  }
  std::string error;
  const auto command = parse_command(line, &error);
  if (!command) {
    answer_now_or_later(session, Held{session.sent, "error " + error, 0});
    return Handled::Done;
  }
  if (const auto* order = std::get_if<NewOrder>(&*command)) {
    const std::uint64_t client_id = order->id.value;
    if (client_id <= session.last_client_id) {
      answer_now_or_later(session, Held{session.sent, "rejected " + std::to_string(client_id) + " id-not-increasing", 0});
      return Handled::Done;
    }
    NewOrder engine_order = *order;
    engine_order.id = OrderId{next_engine_id_};
    if (!send_to_engine(id, client_id, engine_order)) return Handled::NoRoom;
    ++session.sent;
    session.last_client_id = client_id;
    session.live.emplace(client_id, next_engine_id_);
    routes_.emplace(next_engine_id_, Route{id, client_id, order->quantity});
    ++next_engine_id_;
    return Handled::Done;
  }
  const std::uint64_t client_id = std::get<CancelOrder>(*command).id.value;
  const auto live = session.live.find(client_id);
  if (live == session.live.end()) {
    answer_now_or_later(session, Held{session.sent, "rejected " + std::to_string(client_id) + " unknown-order", 0});
    return Handled::Done;
  }
  if (!send_to_engine(id, client_id, CancelOrder{OrderId{live->second}})) return Handled::NoRoom;
  ++session.sent;
  return Handled::Done;
}

bool Gateway::send_to_engine(std::uint64_t session, std::uint64_t client_id, const Command& command) {
  if (!engine_->try_send(Request{next_sequence_, command})) {
    ++stats_.ring_full;
    return false;
  }
  ++next_sequence_;
  in_flight_.push_back(InFlight{session, client_id, command});
  ++stats_.commands;
  return true;
}

void Gateway::answer_now_or_later(Session& session, Held held) {
  ++stats_.gateway_answers;
  session.held.push_back(std::move(held));
  flush_held(session);
}

void Gateway::flush_held(Session& session) {
  while (!session.held.empty() && session.held.front().after <= session.answered) {
    const Held& held = session.held.front();
    session.out += held.text.empty() ? format(feed_.snapshot(held.levels)) : held.text;
    session.out += '\n';
    session.held.pop_front();
  }
}

void Gateway::receive_reports() {
  Report report;
  while (engine_->try_receive(report)) handle_report(report);
}

void Gateway::handle_report(const Report& report) {
  InFlight& front = in_flight_.front();  // the engine answers commands in the order they were sent
  events_.push_back(report.event);
  const std::string client = std::to_string(front.client_id);
  std::visit(Overloaded{
                 [&](const Accepted&) { say(front.session, "accepted " + client); },
                 [&](const Rejected& rejected) {
                   say(front.session, "rejected " + client + " " + std::string(to_string(rejected.reason)));
                 },
                 [&](const Trade& trade) {
                   const std::string fill =
                       " " + std::to_string(trade.quantity.lots) + " @ " + std::to_string(trade.price.ticks);
                   say(front.session, "fill " + client + fill);
                   if (const auto taker = routes_.find(trade.taker.value); taker != routes_.end()) {
                     taker->second.remaining -= trade.quantity;
                   }
                   if (const auto maker = routes_.find(trade.maker.value); maker != routes_.end()) {
                     say(maker->second.session, "fill " + std::to_string(maker->second.client_id) + fill);
                     maker->second.remaining -= trade.quantity;
                     if (maker->second.remaining.lots == 0) forget(trade.maker.value);
                   }
                 },
                 [&](const Cancelled& cancelled) {
                   say(front.session, "cancelled " + client + " " + std::to_string(cancelled.remaining.lots) + " " +
                                          std::string(to_string(cancelled.reason)));
                 },
             },
             report.event);
  if (!report.last) return;
  command_answered(front);
  in_flight_.pop_front();
}

void Gateway::command_answered(const InFlight& done) {
  feed_.apply(done.command, events_, updates_);
  updates_.clear();
  if (const auto* order = std::get_if<NewOrder>(&done.command)) {
    // It rests if it was accepted, is a limit order, and has some left: then it stays live until it fills or is
    // cancelled. Otherwise the gateway is done with its id.
    const auto route = routes_.find(order->id.value);
    const bool rests = std::holds_alternative<Accepted>(events_.front()) && order->type == OrderType::Limit &&
                       route != routes_.end() && route->second.remaining.lots > 0;
    if (!rests) forget(order->id.value);
  } else if (std::holds_alternative<Cancelled>(events_.front())) {
    forget(std::get<CancelOrder>(done.command).id.value);
  }
  events_.clear();
  if (done.session == 0) return;
  const auto session = sessions_.find(done.session);
  if (session == sessions_.end()) return;
  ++session->second.answered;
  flush_held(session->second);
}

void Gateway::forget(std::uint64_t engine_id) {
  const auto route = routes_.find(engine_id);
  if (route == routes_.end()) return;
  if (const auto session = sessions_.find(route->second.session); session != sessions_.end()) {
    session->second.live.erase(route->second.client_id);
  }
  routes_.erase(route);
}

void Gateway::say(std::uint64_t session, const std::string& line) {
  const auto it = sessions_.find(session);
  if (it == sessions_.end()) return;  // the gateway's own command, or a session that has ended
  it->second.out += line;
  it->second.out += '\n';
}

void Gateway::write_out(Session& session) {
  while (session.out_sent < session.out.size()) {
    const ssize_t n = ::send(session.fd, session.out.data() + session.out_sent, session.out.size() - session.out_sent,
                             kSendFlags);
    if (n > 0) {
      session.out_sent += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    drop(session);  // the client has gone (EPIPE, ECONNRESET)
    return;
  }
  if (session.out_sent == session.out.size()) {
    session.out.clear();
    session.out_sent = 0;
  } else if (session.out.size() - session.out_sent > options_.max_unsent_output) {
    drop(session);  // a client that doesn't read its answers
  } else if (session.out_sent >= (std::size_t{1} << 16)) {
    session.out.erase(0, session.out_sent);
    session.out_sent = 0;
  }
}

void Gateway::end_sessions() {
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    Session& session = it->second;
    const bool finished = session.read_closed && session.in.empty() && session.answered == session.sent &&
                          session.held.empty() && session.out.empty();
    if (session.drop || finished) {
      end_session(session);
      it = sessions_.erase(it);
    } else {
      ++it;
    }
  }
}

void Gateway::drop(Session& session) {
  if (session.drop) return;
  session.drop = true;
  ++stats_.dropped_sessions;
}

void Gateway::end_session(Session& session) {
  if (session.drop) write_out(session);  // the error line, if the socket takes it
  for (const auto& [client_id, engine_id] : session.live) pending_cancels_.push_back(engine_id);
  ::close(session.fd);
}

bool Gateway::send_cancels_on_disconnect() {
  while (!pending_cancels_.empty()) {
    if (!send_to_engine(0, 0, CancelOrder{OrderId{pending_cancels_.front()}})) return false;
    pending_cancels_.pop_front();
  }
  return true;
}

}  // namespace orderbook
