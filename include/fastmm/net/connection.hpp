#pragma once
// Resilient WebSocket connection: DNS -> TCP -> TLS -> WS upgrade -> auth -> subscribe ->
// Live, with stale/dead detection, exponential backoff reconnects, and make-before-break
// rollover ahead of the venue's connection lifetime limit (Binance: 24 h).
//
//   Idle -> Resolving -> Connecting -> TlsHandshake -> WsHandshake -> Authenticating
//        -> Subscribing -> Live <-> Stale ; any failure -> Backoff -> Resolving ...
//   close() -> Closing -> Idle (no reconnect)
//
// The handler (static dispatch, see ConnectionHandler) receives state changes and messages.
// on_connected_send_subscriptions() is where the venue layer writes its subscribe/auth
// payloads with send_text(); Authenticating/Subscribing pass through synchronously unless the
// config asks for manual acknowledgement (auth_done()/subscribe_done() from the handler when
// the venue's ack arrives).
//
// Rollover: when max_lifetime_ms elapses, a second session is opened while the first keeps
// delivering; once the new session is Live the old one is closed. Both sessions deliver
// messages during the overlap (market-data consumers must tolerate duplicates, which the
// book-sync FSM does by sequence number). Sends target the newest session from its
// on_ws_open onwards so subscriptions land on the right socket.
#include "fastmm/net/backoff.hpp"
#include "fastmm/net/byte_stream.hpp"
#include "fastmm/net/dns.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/tls_stream.hpp"
#include "fastmm/net/url.hpp"
#include "fastmm/net/ws_client.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fastmm::net {

enum class ConnState : std::uint8_t {
  Idle,
  Resolving,
  Connecting,
  TlsHandshake,
  WsHandshake,
  Authenticating,
  Subscribing,
  Live,
  Stale,  // Live, but nothing received for stale_ms (sub-state of Live)
  Closing,
  Backoff,
};

std::string_view to_string(ConnState s) noexcept;

struct ConnectionConfig {
  std::string url;  // ws:// or wss://host[:port]/path?query
  struct Tls {
    std::string ca_file;    // trust only this bundle (self-signed sim server)
    bool insecure = false;  // skip verification entirely
  } tls;
  std::uint32_t stale_ms = 2000;
  std::uint32_t dead_ms = 10'000;
  BackoffConfig backoff{};
  std::uint64_t max_lifetime_ms = 0;        // 0 = never roll over
  std::uint32_t heartbeat_interval_ms = 0;  // 0 = no client pings
  std::uint32_t connect_timeout_ms = 10'000;
  bool manual_auth = false;       // handler calls auth_done()
  bool manual_subscribe = false;  // handler calls subscribe_done()
  std::string extra_headers;      // "Name: value\r\n" block for the upgrade request
  WsClientConfig ws;
};

// Throws std::invalid_argument on a bad URL; fills `url` (views into cfg.url).
void validate_connection_config(const ConnectionConfig& cfg, Url& url);

template <class H>
concept ConnectionHandler =
    requires(H& h, ConnState s, std::string_view t, std::span<const std::byte> b, std::int64_t ts) {
      h.on_state(s);
      h.on_text(t, ts);
      h.on_binary(b, ts);
      h.on_connected_send_subscriptions();
    };

struct ConnectionStats {
  std::uint64_t connects = 0;    // sessions that reached Live
  std::uint64_t reconnects = 0;  // Backoff entries
  std::uint64_t rollovers = 0;
  std::uint64_t stale_events = 0;
  std::uint64_t dead_events = 0;
  std::uint64_t frames_rx = 0;
  std::uint64_t frames_tx = 0;
  std::uint64_t bytes_rx = 0;
  std::uint64_t bytes_tx = 0;
  std::uint64_t drops = 0;
};

template <ByteStream Stream, ConnectionHandler Handler>
class Connection {
  static constexpr bool kTls = std::is_same_v<Stream, TlsStream<PlainStream>>;
  static_assert(std::is_same_v<Stream, PlainStream> || kTls,
                "Connection supports PlainStream and TlsStream<PlainStream>");

 public:
  // For TLS streams a context is created from cfg.tls unless one is supplied.
  Connection(Reactor& reactor, ConnectionConfig cfg, Handler& handler, TlsContext* tls = nullptr)
      : reactor_(reactor),
        cfg_(std::move(cfg)),
        handler_(handler),
        backoff_(cfg_.backoff),
        resolver_(reactor) {
    validate_connection_config(cfg_, url_);
    if constexpr (kTls) {
      if (tls == nullptr) {
        owned_tls_ = std::make_unique<TlsContext>();
        if (!cfg_.tls.ca_file.empty()) owned_tls_->set_ca_file(cfg_.tls.ca_file);
        owned_tls_->set_insecure(cfg_.tls.insecure);
        tls = owned_tls_.get();
      }
    }
    tls_ = tls;
    host_ = std::string(url_.host);
    char target[1024];
    target_ = std::string(target, url_.request_target(target));
  }
  ~Connection() { teardown(); }
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  // Idle -> Resolving. Reconnects happen automatically until close().
  void connect() {
    if (state_ != ConnState::Idle) return;
    user_closed_ = false;
    backoff_.reset();
    start_resolve();
  }

  // Explicit shutdown: close frame on the live session, then Idle. No reconnect.
  void close() {
    if (state_ == ConnState::Idle) return;
    user_closed_ = true;
    set_state(ConnState::Closing);
    if (active_ && active_->ws.is_open())
      active_->ws.send_close(WsCloseCode::Normal, "client closing");
    teardown();
    set_state(ConnState::Idle);
  }

  // Sends go to the newest session (see rollover note above).
  bool send_text(std::string_view text) noexcept {
    Session* s = send_target();
    if (s == nullptr || !s->ws.is_open()) return false;
    const bool ok = s->ws.send_text(text);
    if (!ok) ++stats_.drops;
    return ok;
  }
  // Coalesces the send_text()/send_binary() calls up to uncork() into one write (WsClient::cork).
  // uncork() returns false when the corked session failed meanwhile (true when none was open).
  void cork() noexcept {
    corked_ = send_target();
    if (corked_ != nullptr) corked_->ws.cork();
  }
  bool uncork() noexcept {
    Session* s = corked_;
    corked_ = nullptr;
    if (s == nullptr) return true;
    if (s != active_.get() && s != pending_.get()) return false;
    return s->ws.uncork();
  }
  bool send_binary(std::span<const std::byte> data) noexcept {
    Session* s = send_target();
    if (s == nullptr || !s->ws.is_open()) return false;
    const bool ok = s->ws.send_binary(data);
    if (!ok) ++stats_.drops;
    return ok;
  }
  bool send_ping() noexcept {
    Session* s = send_target();
    return s != nullptr && s->ws.is_open() && s->ws.send_ping();
  }

  // Manual acknowledgement hooks (see ConnectionConfig::manual_*).
  void auth_done() {
    if (Session* s = newest(); s != nullptr && s->phase == Phase::Authenticating)
      enter_subscribing(*s);
  }
  void subscribe_done() {
    if (Session* s = newest(); s != nullptr && s->phase == Phase::Subscribing) enter_live(*s);
  }

  ConnState state() const noexcept { return state_; }
  bool is_live() const noexcept { return state_ == ConnState::Live || state_ == ConnState::Stale; }
  std::int64_t last_rx_ns() const noexcept { return last_rx_ns_; }
  const Url& url() const noexcept { return url_; }
  const ConnectionConfig& config() const noexcept { return cfg_; }

  // Accumulated counters plus the live sessions' running totals.
  ConnectionStats stats() const noexcept {
    ConnectionStats s = stats_;
    for (const Session* sess : {active_.get(), pending_.get()}) {
      if (sess == nullptr) continue;
      const WsStats& w = sess->ws.stats();
      s.frames_rx += w.frames_rx;
      s.frames_tx += w.frames_tx;
      s.bytes_rx += w.bytes_rx;
      s.bytes_tx += w.bytes_tx;
      s.drops += w.drops;
    }
    return s;
  }

 private:
  enum class Phase : std::uint8_t { Connecting, Authenticating, Subscribing, Live, Dead };

  struct Session;

  // WsClientHandler adapter. It is a separate, complete type because naming
  // WsClient<Stream, Session> from inside Session would check the handler concept against an
  // incomplete class.
  struct SessionEvents {
    Connection* owner;
    Session* self;
    void on_ws_progress(WsProgress p) { owner->on_session_progress(*self, p); }
    void on_ws_open() { owner->on_session_open(*self); }
    void on_ws_text(std::string_view t, std::int64_t ts) { owner->on_session_text(*self, t, ts); }
    void on_ws_binary(std::span<const std::byte> b, std::int64_t ts) {
      owner->on_session_binary(*self, b, ts);
    }
    void on_ws_ping(std::span<const std::byte>) { owner->touch_rx(); }
    void on_ws_pong(std::span<const std::byte>) { owner->touch_rx(); }
    void on_ws_close(std::uint16_t, std::string_view) { owner->on_session_end(*self); }
    void on_ws_error(NetError, std::string_view) { owner->on_session_end(*self); }
  };

  // One WebSocket session; the Connection may briefly own two during rollover. Heap
  // allocated and never moved, so the client's handler reference stays valid.
  struct Session {
    Session(Connection& c, Stream&& stream)
        : events{&c, this}, ws(c.reactor_, std::move(stream), events, c.cfg_.ws) {}

    SessionEvents events;
    WsClient<Stream, SessionEvents> ws;
    Phase phase = Phase::Connecting;
    std::int64_t live_at_ns = 0;
  };

  // ------------------------------------------------------------------------ state helpers
  void set_state(ConnState s) {
    if (state_ == s) return;
    state_ = s;
    handler_.on_state(s);
  }
  Session* newest() noexcept { return pending_ ? pending_.get() : active_.get(); }
  Session* send_target() noexcept {
    if (pending_ && pending_->phase != Phase::Connecting) return pending_.get();
    return active_.get();
  }
  void touch_rx() noexcept { last_rx_ns_ = Reactor::now_ns(); }

  // --------------------------------------------------------------------------- connecting
  void start_resolve() {
    set_state(ConnState::Resolving);
    const std::uint64_t gen = ++resolve_gen_;
    std::weak_ptr<int> alive = alive_;
    resolver_.resolve(host_, url_.port, [this, gen, alive](ResolveResult r) {
      if (alive.expired() || gen != resolve_gen_ || user_closed_) return;
      if (!r.ok()) {
        schedule_backoff();
        return;
      }
      addrs_ = std::move(r.addrs);
      addr_index_ = 0;
      open_session(/*rollover=*/false);
    });
  }

  Stream make_stream(TcpSocket&& sock) {
    if constexpr (kTls) {
      return Stream(*tls_, PlainStream(std::move(sock)), host_);
    } else {
      return Stream(std::move(sock));
    }
  }

  void open_session(bool rollover) {
    const SockAddr& addr = addrs_[addr_index_ % addrs_.size()];
    ++addr_index_;
    TcpSocket sock;
    if (sock.connect(addr) == ConnectStatus::Error) {
      if (rollover) {
        schedule_rollover_retry();
      } else {
        schedule_backoff();
      }
      return;
    }
    sock.set_nodelay(true);
    sock.set_keepalive(true);
    sock.set_rcvbuf(4 * 1024 * 1024);
    auto session = std::make_unique<Session>(*this, make_stream(std::move(sock)));
    Session* raw = session.get();
    (rollover ? pending_ : active_) = std::move(session);
    if (!rollover) set_state(ConnState::Connecting);
    arm_connect_timer();
    if (!raw->ws.start(host_, url_.port, kTls, target_, cfg_.extra_headers)) {
      on_session_end(*raw);
    }
  }

  void arm_connect_timer() {
    cancel(connect_timer_);
    connect_timer_ = reactor_.add_timer_after(ms_to_ns(cfg_.connect_timeout_ms), [this] {
      connect_timer_ = kInvalidTimer;
      if (Session* s = newest(); s != nullptr && s->phase != Phase::Live) on_session_end(*s);
    });
  }

  void on_session_progress(Session& s, WsProgress p) {
    if (&s != active_.get()) return;  // rollover sessions do not change the public state
    if (p == WsProgress::TcpConnected) {
      if constexpr (kTls) set_state(ConnState::TlsHandshake);
    } else {
      set_state(ConnState::WsHandshake);
    }
  }

  void on_session_open(Session& s) {
    s.phase = Phase::Authenticating;
    touch_rx();
    if (&s == active_.get()) set_state(ConnState::Authenticating);
    if (!cfg_.manual_auth) enter_subscribing(s);
  }

  void enter_subscribing(Session& s) {
    s.phase = Phase::Subscribing;
    if (&s == active_.get()) set_state(ConnState::Subscribing);
    handler_.on_connected_send_subscriptions();
    if (s.phase != Phase::Subscribing) return;  // handler closed us
    if (!cfg_.manual_subscribe) enter_live(s);
  }

  void enter_live(Session& s) {
    s.phase = Phase::Live;
    s.live_at_ns = Reactor::now_ns();
    cancel(connect_timer_);
    ++stats_.connects;
    backoff_.reset();
    if (&s == pending_.get()) promote_pending();
    set_state(ConnState::Live);
    arm_health_timer();
    arm_heartbeat_timer();
    arm_lifetime_timer();
  }

  // ------------------------------------------------------------------------------ traffic
  void on_session_text(Session&, std::string_view t, std::int64_t ts) {
    on_rx(ts);
    handler_.on_text(t, ts);
  }
  void on_session_binary(Session&, std::span<const std::byte> b, std::int64_t ts) {
    on_rx(ts);
    handler_.on_binary(b, ts);
  }
  void on_rx(std::int64_t ts) {
    last_rx_ns_ = ts;
    if (state_ == ConnState::Stale) set_state(ConnState::Live);
  }

  // ------------------------------------------------------------------------------ failure
  // Sessions end from inside their own WsClient callbacks, so they are parked in a
  // graveyard and deleted from the reactor loop instead of synchronously.
  void on_session_end(Session& s) {
    if (s.phase == Phase::Dead) return;
    s.phase = Phase::Dead;
    retire(s);
    if (&s == pending_.get()) {
      // A failed rollover keeps the old session; try again after a backoff delay.
      bury(pending_);
      schedule_rollover_retry();
      return;
    }
    bury(active_);
    if (pending_) {
      // The old session died while the new one was still connecting: the new one becomes
      // the (not yet live) active session and the public state follows it.
      active_ = std::move(pending_);
      set_state(active_->phase == Phase::Connecting       ? ConnState::Connecting
                : active_->phase == Phase::Authenticating ? ConnState::Authenticating
                                                          : ConnState::Subscribing);
      return;
    }
    cancel_session_timers();
    if (!user_closed_) schedule_backoff();
  }

  void retire(Session& s) noexcept {
    const WsStats& w = s.ws.stats();
    stats_.frames_rx += w.frames_rx;
    stats_.frames_tx += w.frames_tx;
    stats_.bytes_rx += w.bytes_rx;
    stats_.bytes_tx += w.bytes_tx;
    stats_.drops += w.drops;
    s.ws.close();
  }

  void schedule_backoff() {
    ++stats_.reconnects;
    set_state(ConnState::Backoff);
    cancel(backoff_timer_);
    backoff_timer_ = reactor_.add_timer_after(ms_to_ns(backoff_.next_ms()), [this] {
      backoff_timer_ = kInvalidTimer;
      if (!user_closed_) start_resolve();
    });
  }

  // ----------------------------------------------------------------------------- timers
  void arm_health_timer() {
    cancel(health_timer_);
    const std::uint32_t period =
        std::max<std::uint32_t>(10, std::min(cfg_.stale_ms, cfg_.dead_ms) / 4);
    health_timer_ = reactor_.add_timer_after(ms_to_ns(period), [this] {
      health_timer_ = kInvalidTimer;
      if (!is_live()) return;
      const std::int64_t idle_ms = (Reactor::now_ns() - last_rx_ns_) / 1'000'000;
      if (idle_ms >= cfg_.dead_ms) {
        ++stats_.dead_events;
        on_session_end(*active_);  // forced reconnect through Backoff
        return;
      }
      if (idle_ms >= cfg_.stale_ms && state_ == ConnState::Live) {
        ++stats_.stale_events;
        set_state(ConnState::Stale);
      }
      arm_health_timer();
    });
  }

  void arm_heartbeat_timer() {
    cancel(heartbeat_timer_);
    if (cfg_.heartbeat_interval_ms == 0) return;
    heartbeat_timer_ = reactor_.add_timer_after(ms_to_ns(cfg_.heartbeat_interval_ms), [this] {
      heartbeat_timer_ = kInvalidTimer;
      if (!is_live()) return;
      send_ping();
      arm_heartbeat_timer();
    });
  }

  void arm_lifetime_timer() {
    cancel(lifetime_timer_);
    if (cfg_.max_lifetime_ms == 0) return;
    lifetime_timer_ = reactor_.add_timer_after(ms_to_ns(cfg_.max_lifetime_ms), [this] {
      lifetime_timer_ = kInvalidTimer;
      start_rollover();
    });
  }

  void start_rollover() {
    if (!is_live() || pending_ || addrs_.empty()) return;
    ++stats_.rollovers;
    open_session(/*rollover=*/true);
  }

  void schedule_rollover_retry() {
    cancel(lifetime_timer_);
    lifetime_timer_ = reactor_.add_timer_after(ms_to_ns(backoff_.next_ms()), [this] {
      lifetime_timer_ = kInvalidTimer;
      start_rollover();
    });
  }

  void promote_pending() {
    if (active_) {
      retire(*active_);
      bury(active_);
    }
    active_ = std::move(pending_);
  }

  void bury(std::unique_ptr<Session>& s) {
    if (!s) return;
    graveyard_.push_back(std::move(s));
    if (reap_pending_) return;
    reap_pending_ = true;
    std::weak_ptr<int> alive = alive_;
    reactor_.post([this, alive] {
      if (alive.expired()) return;
      reap_pending_ = false;
      graveyard_.clear();
    });
  }

  void cancel(TimerId& id) noexcept {
    if (id != kInvalidTimer) {
      reactor_.cancel_timer(id);
      id = kInvalidTimer;
    }
  }
  void cancel_session_timers() noexcept {
    cancel(connect_timer_);
    cancel(health_timer_);
    cancel(heartbeat_timer_);
    cancel(lifetime_timer_);
  }
  // close(): sessions go to the graveyard (we may be inside one of their callbacks).
  // Destructor: everything is deleted now; destroying a Connection from inside its own
  // handler callbacks is not supported.
  void teardown() noexcept {
    cancel_session_timers();
    cancel(backoff_timer_);
    ++resolve_gen_;  // orphan any in-flight resolve
    if (pending_) {
      retire(*pending_);
      bury(pending_);
    }
    if (active_) {
      retire(*active_);
      bury(active_);
    }
  }
  static constexpr std::int64_t ms_to_ns(std::uint64_t ms) noexcept {
    return static_cast<std::int64_t>(ms) * 1'000'000;
  }

  Reactor& reactor_;
  ConnectionConfig cfg_;
  Handler& handler_;
  Url url_;
  std::string host_;
  std::string target_;
  std::unique_ptr<TlsContext> owned_tls_;
  TlsContext* tls_ = nullptr;
  ExponentialBackoff backoff_;
  AsyncResolver resolver_;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
  std::uint64_t resolve_gen_ = 0;
  std::vector<SockAddr> addrs_;
  std::size_t addr_index_ = 0;
  std::unique_ptr<Session> active_;
  std::unique_ptr<Session> pending_;
  Session* corked_ = nullptr;  // between cork() and uncork()
  std::vector<std::unique_ptr<Session>> graveyard_;
  bool reap_pending_ = false;
  ConnState state_ = ConnState::Idle;
  bool user_closed_ = false;
  std::int64_t last_rx_ns_ = 0;
  ConnectionStats stats_;
  TimerId connect_timer_ = kInvalidTimer;
  TimerId health_timer_ = kInvalidTimer;
  TimerId heartbeat_timer_ = kInvalidTimer;
  TimerId lifetime_timer_ = kInvalidTimer;
  TimerId backoff_timer_ = kInvalidTimer;
};

}  // namespace fastmm::net
