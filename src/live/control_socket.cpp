#include "fastmm/live/control_socket.hpp"

#include "fastmm/core/log.hpp"
#include "fastmm/core/time.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>

namespace fastmm::live {

namespace {

constexpr std::string_view kUsage =
    "commands (one per datagram; the reply starts with ok or error)\n"
    "  pull [--instrument SYM | --venue NAME]   stop quoting: everywhere, or in that scope\n"
    "  resume [--instrument SYM | --venue NAME] quote again; without a scope it also clears\n"
    "                                           every scoped pull and stops a running flatten\n"
    "  param <name>=<value> ... [--instrument SYM]  new strategy parameters, validated here\n"
    "  limits <key>=<value> ...                 new risk limits (the [risk] keys)\n"
    "  flatten [--instrument SYM] [--max-slippage-bps N]  work the position off, reduce-only\n"
    "  kill                                     trip the kill switch (quotes pulled, all\n"
    "                                           orders cancelled; the position stays)\n"
    "  unkill                                   clear it and quote again (SIGHUP)\n"
    "  stop                                     shut the session down (SIGTERM)\n"
    "  status                                   one line per topic\n"
    "  help                                     this text\n";

std::vector<std::string_view> tokenize(std::string_view s) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    const std::size_t start = i;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r') ++i;
    if (i > start) out.push_back(s.substr(start, i - start));
  }
  return out;
}

std::string error(std::string_view what) {
  return "error " + std::string(what) + "\n";
}
std::string ok(std::string_view what) {
  return "ok " + std::string(what) + "\n";
}

// `--instrument SYM` or `--instrument venue:SYM`. The symbol must name exactly one instrument.
bool find_instrument(const ControlPlane& plane,
                     std::string_view name,
                     InstrumentId& out,
                     std::string& err) {
  if (plane.instruments == nullptr) {
    err = "this session has no instrument table";
    return false;
  }
  std::string_view venue_name;
  std::string_view symbol = name;
  if (const std::size_t colon = name.find(':'); colon != std::string_view::npos) {
    venue_name = name.substr(0, colon);
    symbol = name.substr(colon + 1);
  }
  VenueId venue = VenueId::invalid();
  if (!venue_name.empty()) {
    if (!plane.venue || !plane.venue(venue_name, venue)) {
      err = "no venue named '" + std::string(venue_name) + "'";
      return false;
    }
  }
  const Instrument* found = nullptr;
  std::size_t matches = 0;
  for (const Instrument& inst : *plane.instruments) {
    if (inst.symbol.view() != symbol) continue;
    if (venue.valid() && inst.venue != venue) continue;
    ++matches;
    found = &inst;
  }
  if (matches == 0) {
    err = "no instrument named '" + std::string(name) + "'";
    return false;
  }
  if (matches > 1) {
    err =
        "'" + std::string(name) + "' is on more than one venue; use --instrument <venue>:<symbol>";
    return false;
  }
  out = found->id;
  return true;
}

// The scope flags `pull`, `resume`, `flatten` and `param` share.
struct Scope {
  InstrumentId instrument{};
  VenueId venue = VenueId::invalid();
  std::int64_t slippage_bps = 0;
  ControlPlane::ParamValues values;
};

// Parses the flags of `args`; `key=value` pairs go into scope.values. The error message, empty on
// success.
std::string parse_args(const ControlPlane& plane,
                       std::span<const std::string_view> args,
                       bool allow_venue,
                       bool allow_slippage,
                       bool allow_values,
                       Scope& scope) {
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view a = args[i];
    const auto value = [&](std::string_view& out) {
      if (i + 1 >= args.size()) return false;
      out = args[++i];
      return true;
    };
    std::string_view v;
    if (a == "--instrument" || a == "-i") {
      if (!value(v)) return std::string(a) + " needs a symbol";
      std::string err;
      if (!find_instrument(plane, v, scope.instrument, err)) return err;
    } else if (a == "--venue") {
      if (!allow_venue) return "--venue is not a flag of this command";
      if (!value(v)) return "--venue needs a name";
      if (!plane.venue || !plane.venue(v, scope.venue))
        return "no venue named '" + std::string(v) + "'";
    } else if (a == "--max-slippage-bps") {
      if (!allow_slippage) return "--max-slippage-bps is not a flag of this command";
      if (!value(v)) return "--max-slippage-bps needs a number";
      const auto r = std::from_chars(v.data(), v.data() + v.size(), scope.slippage_bps);
      if (r.ec != std::errc{} || r.ptr != v.data() + v.size() || scope.slippage_bps <= 0)
        return "--max-slippage-bps needs whole basis points > 0";
    } else if (allow_values && a.find('=') != std::string_view::npos) {
      const std::size_t eq = a.find('=');
      if (eq == 0 || eq + 1 == a.size()) return "'" + std::string(a) + "' is not name=value";
      scope.values.emplace_back(std::string(a.substr(0, eq)), std::string(a.substr(eq + 1)));
    } else {
      return "unexpected argument '" + std::string(a) + "'";
    }
  }
  return {};
}

std::string scope_text(const ControlPlane& plane, const Scope& s) {
  if (s.instrument.valid() && plane.instruments != nullptr)
    return " (instrument " + std::string(plane.instruments->get(s.instrument).symbol.view()) + ")";
  if (s.venue.valid()) return " (venue " + std::to_string(s.venue.value) + ")";
  return " (every instrument)";
}

std::string submit_control(ControlPlane& plane,
                           ControlCommand cmd,
                           const Scope& scope,
                           std::uint64_t arg,
                           std::string_view what) {
  if (!plane.submit) return error("this session has no control ring");
  ControlMsg m{};
  init_header(m, EventType::Control, scope.instrument, scope.venue);
  m.command = cmd;
  m.arg = arg;
  m.hdr.recv_ts = wall_now();
  if (!plane.submit(m.hdr)) return error("the control ring is full; try again");
  return ok(std::string(what) + scope_text(plane, scope));
}

// `limits key=value ...`: the session's current limits with the named keys replaced.
std::string parse_limit(RiskLimits& l, std::string_view key, std::string_view value) {
  const auto decimal = [&](auto& field) -> std::string {
    using F = std::remove_reference_t<decltype(field)>;
    const auto v = F::from_decimal(value);
    if (!v) return "'" + std::string(value) + "' is not a decimal";
    field = *v;
    return {};
  };
  const auto integer = [&](auto& field) -> std::string {
    std::int64_t v = 0;
    const auto r = std::from_chars(value.data(), value.data() + value.size(), v);
    if (r.ec != std::errc{} || r.ptr != value.data() + value.size() || v < 0)
      return "'" + std::string(value) + "' is not a whole number >= 0";
    field = static_cast<std::remove_reference_t<decltype(field)>>(v);
    return {};
  };
  if (key == "max_order_qty") return decimal(l.max_order_qty);
  if (key == "max_order_notional") return decimal(l.max_order_notional);
  if (key == "max_position") return decimal(l.max_position);
  if (key == "max_loss") return decimal(l.max_loss);
  if (key == "max_open_orders") return integer(l.max_open_orders);
  if (key == "price_collar_bps") return integer(l.price_collar_bps);
  if (key == "fat_finger_bps") return integer(l.fat_finger_bps);
  if (key == "orders_per_sec") return integer(l.orders_per_sec);
  if (key == "burst") return integer(l.burst);
  if (key == "max_feed_lag_ms") return integer(l.max_feed_lag_ms);
  if (key == "stale_md_ms") {
    std::int64_t ms = 0;
    const auto r = std::from_chars(value.data(), value.data() + value.size(), ms);
    if (r.ec != std::errc{} || r.ptr != value.data() + value.size() || ms < 0)
      return "'" + std::string(value) + "' is not a whole number of milliseconds >= 0";
    l.stale_md = milliseconds(ms);
    return {};
  }
  if (key == "stp") {
    if (value == "true" || value == "1" || value == "on") {
      l.stp = true;
    } else if (value == "false" || value == "0" || value == "off") {
      l.stp = false;
    } else {
      return "'" + std::string(value) + "' is not true or false";
    }
    return {};
  }
  return "unknown limit '" + std::string(key) +
         "' (max_order_qty, max_order_notional, max_position, max_open_orders, price_collar_bps, "
         "fat_finger_bps, stale_md_ms, max_loss, orders_per_sec, burst, stp, max_feed_lag_ms)";
}

}  // namespace

std::string_view control_usage() noexcept {
  return kUsage;
}

std::string control_command(std::string_view request, ControlPlane& plane) {
  const std::vector<std::string_view> tokens = tokenize(request);
  if (tokens.empty()) return error("empty command; `help` lists them");
  const std::string_view verb = tokens[0];
  const std::span<const std::string_view> args(tokens.data() + 1, tokens.size() - 1);

  if (verb == "help") return std::string(kUsage);
  if (verb == "status") {
    if (!plane.status) return error("this session publishes no status");
    return plane.status();
  }
  if (verb == "pull" || verb == "resume" || verb == "flatten") {
    Scope scope;
    const bool flatten = verb == "flatten";
    if (const std::string err = parse_args(plane, args, !flatten, flatten, false, scope);
        !err.empty()) {
      return error(err);
    }
    if (scope.instrument.valid() && scope.venue.valid())
      return error("give --instrument or --venue, not both");
    if (flatten)
      return submit_control(plane,
                            ControlCommand::Flatten,
                            scope,
                            static_cast<std::uint64_t>(scope.slippage_bps),
                            "flatten queued");
    return submit_control(
        plane,
        verb == "pull" ? ControlCommand::PullQuotes : ControlCommand::ResumeQuotes,
        scope,
        0,
        std::string(verb) + " queued");
  }
  if (verb == "param") {
    Scope scope;
    if (const std::string err = parse_args(plane, args, false, false, true, scope); !err.empty())
      return error(err);
    if (scope.values.empty()) return error("param needs at least one name=value");
    if (!plane.params) return error("this session takes no parameter updates");
    const std::string err = plane.params(scope.values, scope.instrument);
    if (!err.empty()) return error(err);
    return ok("param applied" + scope_text(plane, scope));
  }
  if (verb == "limits") {
    Scope scope;
    if (const std::string err = parse_args(plane, args, false, false, true, scope); !err.empty())
      return error(err);
    if (scope.values.empty()) return error("limits needs at least one key=value");
    RiskLimits next = plane.limits;
    for (const auto& [key, value] : scope.values) {
      if (const std::string err = parse_limit(next, key, value); !err.empty()) return error(err);
    }
    if (!plane.submit) return error("this session has no control ring");
    ControlLimitsMsg m{};
    init_header(m, EventType::Control);
    m.command = ControlCommand::SetLimits;
    m.limits = next;
    m.hdr.recv_ts = wall_now();
    if (!plane.submit(m.hdr)) return error("the control ring is full; try again");
    plane.limits = next;
    return ok("limits queued (" + std::to_string(scope.values.size()) + " key(s))");
  }
  if (verb == "kill") {
    if (!args.empty()) return error("kill takes no arguments");
    if (!plane.submit) return error("this session has no control ring");
    ControlMsg m{};
    init_header(m, EventType::Control);
    m.command = ControlCommand::TripKill;
    m.hdr.recv_ts = wall_now();
    if (!plane.submit(m.hdr)) return error("the control ring is full; try again");
    return ok("kill queued (quotes pulled, every order cancelled; the position stays)");
  }
  if (verb == "unkill") {
    if (!args.empty()) return error("unkill takes no arguments");
    if (!plane.clear_kill) return error("this session cannot clear its kill switch");
    if (!plane.clear_kill()) return error("the control ring is full; try again");
    return ok("kill switch cleared");
  }
  if (verb == "stop") {
    if (!args.empty()) return error("stop takes no arguments");
    if (!plane.request_stop) return error("this session cannot be stopped over the socket");
    plane.request_stop();
    return ok("stopping (kill switch, cancel-all, shutdown)");
  }
  return error("unknown command '" + std::string(verb) + "'; `help` lists them");
}

// ---- the listener --------------------------------------------------------------------------

ControlSocket::~ControlSocket() {
  close();
}

int listen_seqpacket(const std::string& path, std::string* error_out) {
  int fd = -1;
  const auto fail = [&](const std::string& what) {
    if (error_out != nullptr) *error_out = what;
    if (fd >= 0) ::close(fd);
    return -1;
  };
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() + 1 > sizeof addr.sun_path)
    return fail("path is longer than " + std::to_string(sizeof addr.sun_path - 1) + " bytes");
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  std::error_code ec;
  if (const std::filesystem::path p(path); p.has_parent_path())
    std::filesystem::create_directories(p.parent_path(), ec);
  // A socket left behind by a session that crashed is ours to replace; anything else is not.
  if (std::filesystem::exists(path, ec)) {
    if (!std::filesystem::is_socket(path, ec)) return fail(path + " exists and is not a socket");
    std::filesystem::remove(path, ec);
  }
  fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return fail(std::string("socket: ") + std::strerror(errno));
  // Between bind() and chmod() the socket would be world-writable with a permissive umask.
  const mode_t old_umask = ::umask(0177);
  const int rc = ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
  const int bind_errno = errno;
  ::umask(old_umask);
  if (rc != 0) return fail("bind " + path + ": " + std::strerror(bind_errno));
  if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0)
    return fail("chmod " + path + ": " + std::strerror(errno));
  if (::listen(fd, 8) != 0) return fail("listen: " + std::string(std::strerror(errno)));
  return fd;
}

bool ControlSocket::open(const std::string& path, std::string* error_out) {
  close();
  listen_fd_ = listen_seqpacket(path, error_out);
  if (listen_fd_ < 0) return false;
  path_ = path;
  return true;
}

void ControlSocket::drop(std::size_t i) noexcept {
  ::close(conns_[i].fd);
  conns_.erase(conns_.begin() + static_cast<std::ptrdiff_t>(i));
}

void ControlSocket::poll(ControlPlane& plane) {
  poll(Handler([&plane](std::string_view request) { return control_command(request, plane); }));
}

void ControlSocket::poll(const Handler& handle) {
  if (listen_fd_ < 0) return;
  const std::int64_t now = steady_now().ns;
  for (std::size_t i = 0; i < kMaxConnections; ++i) {
    const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) break;
    if (conns_.size() >= kMaxConnections) {
      ::close(fd);  // a client that holds a slot without sending must not lock everyone out
      continue;
    }
    conns_.push_back(Conn{fd, now + kIdleTimeoutNs});
  }
  for (std::size_t i = conns_.size(); i > 0; --i) {
    Conn& c = conns_[i - 1];
    char buf[kMaxRequestBytes];
    const ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
    // EAGAIN (== EWOULDBLOCK on Linux): nothing has arrived on this connection yet.
    if (n < 0 && errno == EAGAIN) {
      if (now >= c.deadline_ns) drop(i - 1);
      continue;
    }
    if (n <= 0) {
      drop(i - 1);
      continue;
    }
    ++requests_;
    const std::string reply = handle(std::string_view(buf, static_cast<std::size_t>(n)));
    FASTMM_LOG_INFO("control socket: {} -> {}",
                    std::string_view(buf, static_cast<std::size_t>(n)),
                    std::string_view(reply).substr(0, reply.find('\n')));
    static_cast<void>(::send(c.fd, reply.data(), reply.size(), MSG_NOSIGNAL));
    drop(i - 1);  // one command per connection
  }
}

void ControlSocket::close() noexcept {
  for (const Conn& c : conns_) ::close(c.fd);
  conns_.clear();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (!path_.empty()) {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    path_.clear();
  }
}

}  // namespace fastmm::live
