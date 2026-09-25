#pragma once
// The control socket of a running session: an AF_UNIX SOCK_SEQPACKET listener at
// <journal_dir>/<engine name>.ctl, owned by fastmm-live's control thread (src/live/session.cpp).
//
//   $ fastmm-ctl --name mm pull --instrument BTCUSDT
//   ok pull queued (instrument BTCUSDT)
//
// One datagram is one command line, one datagram back is the reply, so any client that speaks
// SOCK_SEQPACKET works during an incident (socktype=5 is SOCK_SEQPACKET):
//
//   $ echo status | socat - UNIX-CONNECT:runs/mm.ctl,socktype=5
//
// Authorisation is the file system: the socket is created with mode 0600, so whoever can write to
// it can already send the process a signal. It carries no secrets and answers no queries beyond
// what the status file already publishes.
//
// Everything but `param`, `status` and `stop` becomes a message on the engine's control ring, so
// the journal records it and a replay reproduces it (docs/explanation/determinism.md). `param` is
// validated by ParamPublisher on this thread and reaches the engine as a ParamUpdate, which the
// journal records too. `stop` is exactly what SIGTERM does.
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/risk_limits.hpp"
#include "fastmm/core/strong_id.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fastmm::live {

// What a command may do to the session. src/live/session.cpp fills this in; every call runs on the
// control thread, none of them on the engine thread.
struct ControlPlane {
  using ParamValues = std::vector<std::pair<std::string, std::string>>;

  // Puts one message (hdr.len bytes) on the engine's control ring. False: the ring is full.
  std::function<bool(const EventHeader&)> submit;
  // Validates the values against the strategy's schema and publishes them; the error message, or
  // an empty string on success. Unset when the session takes no parameter updates.
  std::function<std::string(const ParamValues&, InstrumentId)> params;
  // The answer to `status`, one topic per line.
  std::function<std::string()> status;
  // Runs the shutdown SIGTERM runs (kill switch, cancel-all on every venue, stop).
  std::function<void()> request_stop;
  // Clears the kill switch and the latched kill state, as SIGHUP does. False: the ring was full.
  std::function<bool()> clear_kill;
  // Venue name -> id, false when no venue has that name.
  std::function<bool(std::string_view, VenueId&)> venue;
  const InstrumentTable* instruments = nullptr;
  // The limits the session runs with. `limits` edits this copy and sends the whole struct, so the
  // keys a command does not name keep the values the session started with.
  RiskLimits limits;
};

// What `fastmm-ctl help` and the socket's `help` answer.
[[nodiscard]] std::string_view control_usage() noexcept;

// Runs one request line and returns the reply. The first word of a reply is `ok` or `error`,
// except for `status` and `help`, which answer with their text.
[[nodiscard]] std::string control_command(std::string_view request, ControlPlane& plane);

// Binds an AF_UNIX SOCK_SEQPACKET listener (non-blocking, close-on-exec) at `path` with mode 0600,
// replacing a socket left behind by a crashed process; a path that is not a socket is never
// removed. The descriptor, or -1 with the reason in `error`. The gateway's socket is made the same
// way (live/gateway.hpp).
[[nodiscard]] int listen_seqpacket(const std::string& path, std::string* error);

// The listener. open() creates the socket, poll() answers whatever has arrived since the last
// call without ever blocking, and close() removes it.
class ControlSocket {
 public:
  ControlSocket() noexcept = default;
  ControlSocket(const ControlSocket&) = delete;
  ControlSocket& operator=(const ControlSocket&) = delete;
  ~ControlSocket();

  // Binds `path` with mode 0600, replacing a socket left behind by a crashed session. False with
  // the reason in `error` (a path that is not a socket is never removed).
  [[nodiscard]] bool open(const std::string& path, std::string* error);
  // Accepts what is waiting, answers every complete request and drops connections that went quiet.
  // Bounded work per call; never blocks.
  void poll(ControlPlane& plane);
  // The same with another command set (fastmm-gateway's): `handle` turns a request into its reply.
  using Handler = std::function<std::string(std::string_view request)>;
  void poll(const Handler& handle);
  void close() noexcept;
  [[nodiscard]] bool is_open() const noexcept { return listen_fd_ >= 0; }
  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::uint64_t requests() const noexcept { return requests_; }

  // A connection that sends nothing is dropped after this long, so a stuck client cannot hold a
  // slot for the whole session.
  static constexpr std::int64_t kIdleTimeoutNs = 2'000'000'000;
  static constexpr std::size_t kMaxConnections = 8;
  static constexpr std::size_t kMaxRequestBytes = 4096;

 private:
  struct Conn {
    int fd = -1;
    std::int64_t deadline_ns = 0;
  };
  void drop(std::size_t i) noexcept;

  int listen_fd_ = -1;
  std::string path_;
  std::vector<Conn> conns_;
  std::uint64_t requests_ = 0;
};

}  // namespace fastmm::live
