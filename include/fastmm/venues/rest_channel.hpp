#pragma once
// RestChannel: reactor-driven keep-alive HTTP(S) for the venue requests that run on the net thread
// (depth snapshots, openOrders, execution replays, listenKey keepalive, and the order traffic that
// goes over REST: orders falling back from the WebSocket, cancel-all, dead man's switch refreshes).
//
// HTTP/1.1 answers one request at a time per connection, so a channel with one connection makes a
// cancel wait for every snapshot and replay page queued ahead of it. The channel therefore has two
// lanes, each with its own connection and queue:
//
//   RestLane::Control  everything else: reference data, snapshots, reconciliation, replays
//   RestLane::Orders   orders, cancels and the requests that keep orders safe (cancel-all, the
//                      dead man's switch): they wait for nothing but each other
//
// A lane hands its client one request at a time and keeps the rest. When a connection that was up
// fails, only the request on it fails; the ones behind it go out on the next connection. When a
// connection cannot be made (refused, TLS failure, connect timeout, no address), every request of
// the lane fails with that error and the host is resolved again for the next attempt, so a venue
// that moved its addresses is found again. Lookups run off the net thread (net::AsyncResolver):
// the first starts when the channel is made.
//
// Requests are std::function/std::string based: control path only.
#include "fastmm/net/dns.hpp"
#include "fastmm/net/http_client.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/source_address.hpp"
#include "fastmm/net/tls_stream.hpp"
#include "fastmm/net/url.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fastmm::venues {

struct RestChannelConfig {
  std::string base_url;  // https://host[:port][/prefix]
  std::string ca_file;
  bool insecure_tls = false;
  std::uint32_t timeout_ms = 5000;  // per request once written; the connect and the lookup too
  std::size_t max_queue = 8;        // per lane, the request in flight included
  net::SourceAddress source;        // where the connection leaves this host from; empty: anywhere
};

// Queue room for a connector's REST channel with `symbols` subscribed: every connector fans some
// requests out per symbol (cancel-all, the USDⓈ-M countdown, execution and funding replays), and
// those can overlap, on top of a fixed set (reconciliation, clock, listen key, the dead man's
// switch, orders falling back to REST). The queue is a deque: room costs nothing until used.
[[nodiscard]] constexpr std::size_t rest_queue_for(std::size_t symbols) noexcept {
  return 32 + 4 * symbols;
}

// The RestChannelConfig every connector config spells the same way.
template <class Cfg>
[[nodiscard]] RestChannelConfig rest_channel_config(const Cfg& cfg, std::size_t symbols) {
  RestChannelConfig rc;
  rc.base_url = cfg.rest_url;
  rc.ca_file = cfg.ca_file;
  rc.insecure_tls = cfg.insecure_tls;
  rc.timeout_ms = cfg.http_timeout_ms;
  rc.max_queue = rest_queue_for(symbols);
  if constexpr (requires { cfg.source; }) rc.source = cfg.source;
  return rc;
}

enum class RestLane : std::uint8_t { Control = 0, Orders = 1 };

class RestChannel {
 public:
  using Callback = net::HttpResponseCallback;
  using PlainClient = net::HttpClient<net::PlainStream>;
  using TlsClient = net::HttpClient<net::TlsStream<net::PlainStream>>;

  RestChannel(net::Reactor& reactor, RestChannelConfig cfg)
      : reactor_(reactor), cfg_(std::move(cfg)), resolver_(reactor) {
    const auto url = net::Url::parse(cfg_.base_url);
    if (!url) throw std::invalid_argument("RestChannel: bad base url '" + cfg_.base_url + "'");
    host_ = std::string(url->host);
    port_ = url->port;
    use_tls_ = url->tls;
    prefix_ = url->path == "/" ? std::string{} : std::string(url->path);
    while (!prefix_.empty() && prefix_.back() == '/') prefix_.pop_back();
    if (use_tls_) tls_ctx_ = net::TlsContext::shared_client(cfg_.ca_file, cfg_.insecure_tls);
    start_resolve();
  }
  ~RestChannel() {
    cancel_resolve_timer();
    reset();
  }
  RestChannel(const RestChannel&) = delete;
  RestChannel& operator=(const RestChannel&) = delete;

  // `target` is the path plus query without the base prefix. The callback runs on the reactor
  // thread, once; a transport failure arrives as HttpResponse::error != None. Returns false (and
  // never calls back) when the lane's queue is full.
  bool request(std::string_view method,
               std::string_view target,
               std::string_view extra_headers,
               std::string_view body,
               Callback cb,
               RestLane lane = RestLane::Control) {
    Pending p;
    p.target = prefix_ + std::string(target);
    return enqueue(lane, method, extra_headers, body, std::move(p), std::move(cb));
  }

  // The same, with the target (without the base prefix) made when the request is written, not
  // when it is queued: a signed query is stamped and signed then, however long it waited in its
  // lane. Empty: not sent, the callback gets NetError::Canceled.
  bool request(std::string_view method,
               net::HttpTargetMaker make_target,
               std::string_view extra_headers,
               std::string_view body,
               Callback cb,
               RestLane lane = RestLane::Control) {
    if (!make_target) return false;
    Pending p;
    p.make_target = [prefix = prefix_, make = std::move(make_target)] {
      std::string target = make();
      return target.empty() ? target : prefix + target;
    };
    return enqueue(lane, method, extra_headers, body, std::move(p), std::move(cb));
  }

  // Closes both connections; every request waiting or in flight gets NetError::Canceled. The
  // channel stays usable: the next request connects again. Not from inside one of its callbacks.
  void reset() noexcept {
    for (Lane& l : lanes_) {
      std::deque<Pending> waiting;
      waiting.swap(l.waiting);
      for (Pending& p : waiting) fail(p, net::NetError::Canceled, "closed");
      l.plain.reset();
      l.tls.reset();
      l.busy = false;
    }
  }

  // For a subscription that grew after connect: see rest_queue_for().
  void set_max_queue(std::size_t n) noexcept { cfg_.max_queue = n; }

  // Requests of `lane` waiting or in flight.
  [[nodiscard]] std::size_t queued(RestLane lane = RestLane::Control) const noexcept {
    const Lane& l = lanes_[index(lane)];
    return l.waiting.size() + (l.busy ? 1 : 0);
  }
  [[nodiscard]] std::size_t max_queue() const noexcept { return cfg_.max_queue; }
  // The lane has a connection, made or being made.
  [[nodiscard]] bool connected(RestLane lane) const noexcept {
    const Lane& l = lanes_[index(lane)];
    return l.plain != nullptr || l.tls != nullptr;
  }
  [[nodiscard]] std::uint64_t requests() const noexcept { return requests_; }
  [[nodiscard]] std::uint64_t errors() const noexcept { return errors_; }
  [[nodiscard]] std::uint64_t connects() const noexcept { return connects_; }
  [[nodiscard]] std::uint64_t lookups() const noexcept { return lookups_; }
  [[nodiscard]] const std::string& host() const noexcept { return host_; }
  [[nodiscard]] bool tls() const noexcept { return use_tls_; }

 private:
  static constexpr std::size_t kLanes = 2;

  struct Pending {
    std::string method;
    std::string target;  // with the prefix; empty when make_target makes it
    net::HttpTargetMaker make_target;
    std::string headers;
    std::string body;
    Callback cb;
  };

  struct Lane {
    std::deque<Pending> waiting;  // not handed to a client yet
    std::unique_ptr<PlainClient> plain;
    std::unique_ptr<TlsClient> tls;
    bool busy = false;  // a request is with the client, its callback has not run
  };

  static constexpr std::size_t index(RestLane lane) noexcept {
    return static_cast<std::size_t>(lane);
  }

  bool enqueue(RestLane lane,
               std::string_view method,
               std::string_view headers,
               std::string_view body,
               Pending&& p,
               Callback&& cb) {
    if (queued(lane) >= cfg_.max_queue) {
      ++errors_;
      return false;
    }
    p.method = method;
    p.headers = headers;
    p.body = body;
    p.cb = std::move(cb);
    Lane& l = lanes_[index(lane)];
    l.waiting.push_back(std::move(p));
    ++requests_;
    // An idle open connection takes the request now, also from inside a callback of its own
    // request (HttpClient takes the next one there). Anything else goes through the reactor: a
    // connection is never replaced from inside its own callback, and a lookup or a connect that
    // fails never calls back before request() has returned.
    const bool ready = (l.plain && l.plain->is_ready()) || (l.tls && l.tls->is_ready());
    if (ready && !l.busy) {
      pump(lane);
    } else if (!l.busy) {
      schedule_pump();
    }
    return true;
  }

  void fail(Pending& p, net::NetError err, std::string_view detail) {
    ++errors_;
    net::HttpResponse r;
    r.error = err;
    r.error_detail = detail;
    r.rx_ts = net::Reactor::now_ns();
    if (p.cb) p.cb(r);
  }

  // Every request waiting in `lane` fails with `err` (the connection cannot be made).
  void fail_waiting(Lane& l, net::NetError err, std::string_view detail) {
    std::deque<Pending> waiting;
    waiting.swap(l.waiting);
    for (Pending& p : waiting) fail(p, err, detail);
  }

  void schedule_pump() {
    if (pump_pending_) return;
    pump_pending_ = true;
    std::weak_ptr<int> alive = alive_;
    reactor_.post([this, alive] {
      if (alive.expired()) return;
      pump_pending_ = false;
      pump(RestLane::Control);
      pump(RestLane::Orders);
    });
  }

  // Hands the lane's next request to its client, connecting first when it has none. From inside a
  // client's callback only when that client is open and idle (enqueue), so it is never replaced
  // there.
  void pump(RestLane lane) {
    Lane& l = lanes_[index(lane)];
    if (l.busy || l.waiting.empty()) return;
    if (l.plain && l.plain->state() == net::HttpClientState::Closed) l.plain.reset();
    if (l.tls && l.tls->state() == net::HttpClientState::Closed) l.tls.reset();
    if (!l.plain && !l.tls && !open(lane)) return;
    Pending p = std::move(l.waiting.front());
    l.waiting.pop_front();
    l.busy = true;  // before the hand-over: a request the client refuses calls back at once
    auto cb = [this, lane, alive = std::weak_ptr<int>(alive_), user = std::move(p.cb)](
                  const net::HttpResponse& r) {
      if (alive.expired()) return;
      done(lane, r, user);
    };
    const bool taken = p.make_target ? hand(l, p.method, std::move(p.make_target), p, cb)
                                     : hand(l, p.method, std::string_view(p.target), p, cb);
    if (!taken) {
      l.busy = false;
      ++errors_;
      net::HttpResponse r;
      r.error = net::NetError::InvalidState;
      r.error_detail = "connection refused the request";
      cb(r);
    }
  }

  template <class Target, class Cb>
  bool hand(Lane& l, std::string_view method, Target&& target, const Pending& p, Cb& cb) {
    if (l.plain)
      return l.plain->request(method, std::forward<Target>(target), p.headers, p.body, cb);
    return l.tls->request(method, std::forward<Target>(target), p.headers, p.body, cb);
  }

  // The client answered the lane's request, or failed it.
  void done(RestLane lane, const net::HttpResponse& r, const Callback& user) {
    Lane& l = lanes_[index(lane)];
    l.busy = false;
    if (r.error != net::NetError::None) {
      ++errors_;
      // A connection that never came up fails everything behind it as well, and the next attempt
      // looks the host up again.
      const bool closed = (l.plain && l.plain->state() == net::HttpClientState::Closed) ||
                          (l.tls && l.tls->state() == net::HttpClientState::Closed);
      const bool established =
          (l.plain && l.plain->established()) || (l.tls && l.tls->established());
      if (closed && !established && r.error != net::NetError::Canceled) {
        addrs_stale_ = true;
        if (user) user(r);
        fail_waiting(l, r.error, r.error_detail);
        schedule_pump();
        return;
      }
    }
    if (user) user(r);
    schedule_pump();
  }

  // Makes the lane's connection. False when there is none yet: the lookup is running (its answer
  // pumps the lanes) or the connect failed (the lane's requests have failed).
  bool open(RestLane lane) {
    Lane& l = lanes_[index(lane)];
    if (addrs_.empty() || addrs_stale_) {
      wait_for_lookup();
      return false;
    }
    net::TcpSocket sock;
    const net::SockAddr& addr = addrs_[addr_index_++ % addrs_.size()];
    const std::optional<net::SockAddr> local = net::source_bind_address(cfg_.source, addr.family());
    if (sock.connect(addr, local ? &*local : nullptr) == net::ConnectStatus::Error) {
      addrs_stale_ = true;
      fail_waiting(l, net::NetError::Syscall, "connect failed");
      return false;
    }
    sock.set_nodelay(true);
    net::HttpClientConfig hc;
    hc.timeout_ms = cfg_.timeout_ms;
    hc.max_queue = 1;  // the lane keeps the queue
    ++connects_;
    bool started = false;
    if (use_tls_) {
      l.tls = std::make_unique<TlsClient>(
          reactor_,
          net::TlsStream<net::PlainStream>(*tls_ctx_, net::PlainStream(std::move(sock)), host_),
          host_,
          port_,
          true,
          hc);
      started = l.tls->start();
      if (!started) l.tls.reset();
    } else {
      l.plain = std::make_unique<PlainClient>(
          reactor_, net::PlainStream(std::move(sock)), host_, port_, false, hc);
      started = l.plain->start();
      if (!started) l.plain.reset();
    }
    if (!started) {
      fail_waiting(l, net::NetError::Syscall, "connection not registered");
      return false;
    }
    // A connect refused at once (loopback) closes the client inside start().
    const bool closed = (l.plain && l.plain->state() == net::HttpClientState::Closed) ||
                        (l.tls && l.tls->state() == net::HttpClientState::Closed);
    if (closed) {
      addrs_stale_ = true;
      fail_waiting(l, net::NetError::Syscall, "connect failed");
      return false;
    }
    return true;
  }

  // Looks the host up unless a lookup is running. The constructor's lookup has nothing waiting for
  // it; a lane that waits arms the timeout (wait_for_lookup).
  void start_resolve() {
    if (resolving_) return;
    resolving_ = true;
    ++lookups_;
    std::weak_ptr<int> alive = alive_;
    resolver_.resolve(host_, port_, [this, alive](net::ResolveResult r) {
      if (alive.expired()) return;
      resolving_ = false;
      cancel_resolve_timer();
      if (r.ok()) {
        net::keep_reachable(cfg_.source, r.addrs);
        if (!r.addrs.empty()) addrs_ = std::move(r.addrs);
      }
      if (addrs_.empty()) {
        const std::string_view why =
            r.ok() ? "no address of the source's family" : "host lookup failed";
        for (Lane& l : lanes_) fail_waiting(l, net::NetError::Resolve, why);
        return;
      }
      // A failed refresh keeps the addresses it had: the lanes try them again.
      addrs_stale_ = false;
      pump(RestLane::Control);
      pump(RestLane::Orders);
    });
  }

  // A lookup that hangs (an unreachable DNS server) fails the requests waiting for it after the
  // request timeout; its answer, whenever it comes, is still kept for the next ones.
  void wait_for_lookup() {
    start_resolve();
    if (resolve_timer_ != net::kInvalidTimer) return;
    resolve_timer_ =
        reactor_.add_timer_after(static_cast<std::int64_t>(cfg_.timeout_ms) * 1'000'000, [this] {
          resolve_timer_ = net::kInvalidTimer;
          if (!resolving_) return;
          for (Lane& l : lanes_) fail_waiting(l, net::NetError::Timeout, "host lookup timed out");
        });
  }

  void cancel_resolve_timer() noexcept {
    if (resolve_timer_ != net::kInvalidTimer) {
      reactor_.cancel_timer(resolve_timer_);
      resolve_timer_ = net::kInvalidTimer;
    }
  }

  net::Reactor& reactor_;
  RestChannelConfig cfg_;
  std::string host_;
  std::uint16_t port_ = 0;
  bool use_tls_ = false;
  std::string prefix_;
  std::shared_ptr<const net::TlsContext> tls_ctx_;
  std::array<Lane, kLanes> lanes_;
  net::AsyncResolver resolver_;
  std::vector<net::SockAddr> addrs_;
  std::size_t addr_index_ = 0;
  bool addrs_stale_ = false;
  bool resolving_ = false;
  net::TimerId resolve_timer_ = net::kInvalidTimer;
  bool pump_pending_ = false;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
  std::uint64_t requests_ = 0;
  std::uint64_t errors_ = 0;
  std::uint64_t connects_ = 0;
  std::uint64_t lookups_ = 0;
};

}  // namespace fastmm::venues
