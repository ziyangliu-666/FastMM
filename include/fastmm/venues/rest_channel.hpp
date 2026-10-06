#pragma once
// RestChannel: reactor-driven keep-alive HTTP(S) client with lazy (re)connection, for the
// venue control requests that run on the net thread (depth snapshots, openOrders, listenKey
// keepalive, order-entry REST fallback). net::HttpClient is single-connection and does not
// reconnect; this wrapper recreates it on demand and re-resolves the host when a connect
// fails. Requests are std::function/std::string based: control path only.
#include "fastmm/net/dns.hpp"
#include "fastmm/net/http_client.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/source_address.hpp"
#include "fastmm/net/tls_stream.hpp"
#include "fastmm/net/url.hpp"

#include <cstdint>
#include <memory>
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
  std::uint32_t timeout_ms = 5000;
  std::size_t max_queue = 8;
  net::SourceAddress source;  // where the connection leaves this host from; empty: anywhere
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

class RestChannel {
 public:
  using Callback = net::HttpResponseCallback;
  using PlainClient = net::HttpClient<net::PlainStream>;
  using TlsClient = net::HttpClient<net::TlsStream<net::PlainStream>>;

  RestChannel(net::Reactor& reactor, RestChannelConfig cfg)
      : reactor_(reactor), cfg_(std::move(cfg)) {
    const auto url = net::Url::parse(cfg_.base_url);
    if (!url) throw std::invalid_argument("RestChannel: bad base url '" + cfg_.base_url + "'");
    host_ = std::string(url->host);
    port_ = url->port;
    use_tls_ = url->tls;
    prefix_ = url->path == "/" ? std::string{} : std::string(url->path);
    while (!prefix_.empty() && prefix_.back() == '/') prefix_.pop_back();
    if (use_tls_) {
      tls_ctx_ = std::make_unique<net::TlsContext>();
      if (!cfg_.ca_file.empty()) tls_ctx_->set_ca_file(cfg_.ca_file);
      tls_ctx_->set_insecure(cfg_.insecure_tls);
    }
  }
  ~RestChannel() { reset(); }
  RestChannel(const RestChannel&) = delete;
  RestChannel& operator=(const RestChannel&) = delete;

  // `target` is the path plus query without the base prefix. The callback runs on the
  // reactor thread; a transport failure arrives as HttpResponse::error != None. Returns
  // false when the request could not even be queued (resolve/connect failure, queue full).
  bool request(std::string_view method,
               std::string_view target,
               std::string_view extra_headers,
               std::string_view body,
               Callback cb) {
    if (!ensure_client()) return false;
    const std::string full = prefix_ + std::string(target);
    ++requests_;
    auto wrapped = wrap(std::move(cb));
    const bool ok = plain_ ? plain_->request(method, full, extra_headers, body, wrapped)
                           : tls_->request(method, full, extra_headers, body, wrapped);
    if (!ok) ++errors_;
    return ok;
  }

  // The same, with the target (without the base prefix) made when the request is written, not
  // when it is queued: a signed query is stamped and signed then, however long it waited behind
  // the requests ahead of it on the one connection (net::HttpClient). Empty: not sent, the
  // callback gets NetError::Canceled.
  bool request(std::string_view method,
               net::HttpTargetMaker make_target,
               std::string_view extra_headers,
               std::string_view body,
               Callback cb) {
    if (!make_target || !ensure_client()) return false;
    ++requests_;
    net::HttpTargetMaker full = [prefix = prefix_, make = std::move(make_target)] {
      std::string target = make();
      return target.empty() ? target : prefix + target;
    };
    auto wrapped = wrap(std::move(cb));
    const bool ok = plain_ ? plain_->request(method, full, extra_headers, body, wrapped)
                           : tls_->request(method, full, extra_headers, body, wrapped);
    if (!ok) ++errors_;
    return ok;
  }

  // Drops the connection (deferred to the reactor loop so it is safe from callbacks).
  void schedule_reset() {
    if (reset_pending_) return;
    reset_pending_ = true;
    std::weak_ptr<int> alive = alive_;
    reactor_.post([this, alive] {
      if (alive.expired()) return;
      reset_pending_ = false;
      reset();
    });
  }
  void reset() noexcept {
    plain_.reset();
    tls_.reset();
  }

  // For a subscription that grew after connect: see rest_queue_for().
  void set_max_queue(std::size_t n) noexcept {
    cfg_.max_queue = n;
    if (plain_) plain_->set_max_queue(n);
    if (tls_) tls_->set_max_queue(n);
  }

  [[nodiscard]] bool connected() const noexcept { return plain_ || tls_; }
  // Requests queued on the connection, the one in flight included.
  [[nodiscard]] std::size_t queued() const noexcept {
    if (plain_) return plain_->queued();
    return tls_ ? tls_->queued() : 0;
  }
  [[nodiscard]] std::size_t max_queue() const noexcept { return cfg_.max_queue; }
  [[nodiscard]] std::uint64_t requests() const noexcept { return requests_; }
  [[nodiscard]] std::uint64_t errors() const noexcept { return errors_; }
  [[nodiscard]] const std::string& host() const noexcept { return host_; }
  [[nodiscard]] bool tls() const noexcept { return use_tls_; }

 private:
  Callback wrap(Callback cb) {
    return [this, cb = std::move(cb)](const net::HttpResponse& r) {
      if (r.error != net::NetError::None) {
        ++errors_;
        // Canceled: the client closed already, or a withdrawn target that left it usable.
        if (r.error != net::NetError::Canceled) schedule_reset();
      }
      if (cb) cb(r);
    };
  }

  bool ensure_client() {
    if (plain_ && plain_->state() == net::HttpClientState::Closed) plain_.reset();
    if (tls_ && tls_->state() == net::HttpClientState::Closed) tls_.reset();
    if (plain_ || tls_) return true;
    if (addrs_.empty() || addr_gen_ != resolve_gen_) {
      // Blocking resolve on the net thread: only on (re)connect, cached afterwards.
      const net::ResolveResult r = net::resolve_sync(host_, port_);
      if (!r.ok()) {
        ++errors_;
        return false;
      }
      addrs_ = r.addrs;
      net::keep_reachable(cfg_.source, addrs_);
      if (addrs_.empty()) {
        ++errors_;
        return false;
      }
      addr_gen_ = resolve_gen_;
    }
    net::TcpSocket sock;
    const net::SockAddr& addr = addrs_[addr_index_++ % addrs_.size()];
    const std::optional<net::SockAddr> local = net::source_bind_address(cfg_.source, addr.family());
    if (sock.connect(addr, local ? &*local : nullptr) == net::ConnectStatus::Error) {
      ++errors_;
      ++resolve_gen_;  // re-resolve next time
      return false;
    }
    sock.set_nodelay(true);
    net::HttpClientConfig hc;
    hc.timeout_ms = cfg_.timeout_ms;
    hc.max_queue = cfg_.max_queue;
    if (use_tls_) {
      tls_ = std::make_unique<TlsClient>(
          reactor_,
          net::TlsStream<net::PlainStream>(*tls_ctx_, net::PlainStream(std::move(sock)), host_),
          host_,
          port_,
          true,
          hc);
      if (!tls_->start()) {
        tls_.reset();
        return false;
      }
    } else {
      plain_ = std::make_unique<PlainClient>(
          reactor_, net::PlainStream(std::move(sock)), host_, port_, false, hc);
      if (!plain_->start()) {
        plain_.reset();
        return false;
      }
    }
    return true;
  }

  net::Reactor& reactor_;
  RestChannelConfig cfg_;
  std::string host_;
  std::uint16_t port_ = 0;
  bool use_tls_ = false;
  std::string prefix_;
  std::unique_ptr<net::TlsContext> tls_ctx_;
  std::unique_ptr<PlainClient> plain_;
  std::unique_ptr<TlsClient> tls_;
  std::vector<net::SockAddr> addrs_;
  std::size_t addr_index_ = 0;
  std::uint64_t resolve_gen_ = 0;
  std::uint64_t addr_gen_ = ~0ULL;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
  bool reset_pending_ = false;
  std::uint64_t requests_ = 0;
  std::uint64_t errors_ = 0;
};

}  // namespace fastmm::venues
