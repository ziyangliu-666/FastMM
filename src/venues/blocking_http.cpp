#include "fastmm/venues/blocking_http.hpp"

#include "fastmm/net/dns.hpp"
#include "fastmm/net/http_client.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/tls_stream.hpp"
#include "fastmm/net/url.hpp"

#include <optional>
#include <stdexcept>
#include <system_error>

namespace fastmm::venues {

namespace {

using PlainClient = net::HttpClient<net::PlainStream>;
using TlsClient = net::HttpClient<net::TlsStream<net::PlainStream>>;

constexpr std::int64_t kNsPerMs = 1'000'000;

// Sent again on a new connection when the reused one was gone before any answer: a request that
// means the same however often it arrives (RFC 9110, 9.2.2). POST may not.
bool idempotent(std::string_view method) noexcept {
  return method == "GET" || method == "HEAD" || method == "DELETE" || method == "PUT" ||
         method == "OPTIONS";
}

}  // namespace

// Declared in this order so the clients go before the reactor they are registered with.
struct BlockingHttp::Session {
  net::Reactor reactor;
  std::shared_ptr<const net::TlsContext> tls;  // the process's, from the first TLS connection
  std::unique_ptr<PlainClient> plain;
  std::unique_ptr<TlsClient> secure;
  std::int64_t idle_since_ns = 0;

  [[nodiscard]] bool open() const noexcept {
    return (plain && plain->state() != net::HttpClientState::Closed) ||
           (secure && secure->state() != net::HttpClientState::Closed);
  }
  void drop() noexcept {
    plain.reset();
    secure.reset();
  }
};

BlockingHttp::BlockingHttp(std::string base_url, BlockingHttpOptions opts)
    : base_url_(std::move(base_url)), opts_(std::move(opts)) {
  const auto url = net::Url::parse(base_url_);
  if (!url) throw std::invalid_argument("BlockingHttp: bad base url '" + base_url_ + "'");
  if (url->scheme != "http" && url->scheme != "https")
    throw std::invalid_argument("BlockingHttp: scheme must be http(s): '" + base_url_ + "'");
  host_ = std::string(url->host);
  port_ = url->port;
  tls_ = url->tls;
  base_path_ = url->path == "/" ? std::string{} : std::string(url->path);
  while (!base_path_.empty() && base_path_.back() == '/') base_path_.pop_back();
}

BlockingHttp::~BlockingHttp() = default;

std::string BlockingHttp::connect() {
  if (addrs_.empty()) {
    net::ResolveResult res = net::resolve_sync(host_, port_);
    if (!res.ok()) return "resolve failed: " + res.error_text();
    net::keep_reachable(opts_.source, res.addrs);
    if (res.addrs.empty()) return "no address of " + host_ + " in the family of the source address";
    addrs_ = std::move(res.addrs);
  }
  net::TcpSocket sock;
  const net::SockAddr& addr = addrs_.front();
  const std::optional<net::SockAddr> local = net::source_bind_address(opts_.source, addr.family());
  if (sock.connect(addr, local ? &*local : nullptr) == net::ConnectStatus::Error) {
    addrs_.clear();  // looked up again next time
    return local ? "connect failed from " + local->to_string() + ": " +
                       std::error_code(sock.last_error(), std::generic_category()).message()
                 : std::string("connect failed");
  }
  sock.set_nodelay(true);
  Session& s = *session_;
  net::HttpClientConfig cfg;
  cfg.timeout_ms = opts_.timeout_ms;
  cfg.recv_capacity = opts_.recv_capacity;
  cfg.max_queue = 1;
  ++connects_;
  bool started = false;
  if (tls_) {
    if (s.tls == nullptr) s.tls = net::TlsContext::shared_client(opts_.ca_file, opts_.insecure_tls);
    s.secure = std::make_unique<TlsClient>(
        s.reactor,
        net::TlsStream<net::PlainStream>(*s.tls, net::PlainStream(std::move(sock)), host_),
        host_,
        port_,
        true,
        cfg);
    started = s.secure->start();
  } else {
    s.plain = std::make_unique<PlainClient>(
        s.reactor, net::PlainStream(std::move(sock)), host_, port_, false, cfg);
    started = s.plain->start();
  }
  if (!started) {
    s.drop();
    return "cannot register socket";
  }
  return {};
}

HttpReply BlockingHttp::attempt(std::string_view method,
                                std::string_view target,
                                std::string_view extra_headers,
                                std::string_view body,
                                bool& reused,
                                bool& lost) {
  HttpReply reply;
  lost = false;
  if (session_ == nullptr) session_ = std::make_unique<Session>();
  Session& s = *session_;
  if (s.open()) {
    // Whatever the server sent meanwhile (its close of an idle connection) is read first.
    s.reactor.run_once(0);
    if (!s.open() || net::Reactor::now_ns() - s.idle_since_ns > kReuseIdleMs * kNsPerMs) s.drop();
  }
  reused = s.open();
  if (!reused) {
    s.drop();
    if (std::string err = connect(); !err.empty()) {
      reply.error = std::move(err);
      return reply;
    }
  }
  bool done = false;
  auto on_reply = [&](const net::HttpResponse& r) {
    done = true;
    lost = r.error == net::NetError::Closed || r.error == net::NetError::Syscall;
    if (r.error != net::NetError::None) {
      reply.error = std::string(net::to_string(r.error)) + ": " + std::string(r.error_detail);
      return;
    }
    reply.status = r.status;
    reply.body = std::string(r.body);
    for (const net::HttpHeader& h : r.headers.view())
      reply.headers.emplace_back(std::string(h.name), std::string(h.value));
  };
  const std::string full_target = base_path_ + std::string(target);
  const bool queued = s.secure
                          ? s.secure->request(method, full_target, extra_headers, body, on_reply)
                          : s.plain->request(method, full_target, extra_headers, body, on_reply);
  if (!queued) {
    // start() drives the non-blocking connect; on loopback a refused connection fails at once and
    // closes the client before the request can be queued.
    reply.error =
        s.open() ? "request not queued" : "connect failed (connection refused or unreachable)";
    if (!s.open()) addrs_.clear();
    s.drop();
    return reply;
  }
  const std::int64_t deadline =
      net::Reactor::now_ns() +
      static_cast<std::int64_t>(opts_.timeout_ms + opts_.connect_timeout_ms) * kNsPerMs;
  // `done` is set by the response callback, which run_once() invokes from inside the loop.
  while (!done) {  // NOLINT(bugprone-infinite-loop)
    if (net::Reactor::now_ns() > deadline) {
      reply.error = "timeout";
      break;
    }
    s.reactor.run_once(50);
  }
  // A connection whose request did not finish cleanly cannot carry the next one.
  if (!done || !reply.error.empty()) s.drop();
  s.idle_since_ns = net::Reactor::now_ns();
  return reply;
}

HttpReply BlockingHttp::request(std::string_view method,
                                std::string_view target,
                                std::string_view extra_headers,
                                std::string_view body) {
  bool reused = false;
  bool lost = false;
  HttpReply reply = attempt(method, target, extra_headers, body, reused, lost);
  // The server closed the kept connection as the request went out: nothing came back, so it
  // goes again on a fresh connection when sending it twice means the same as once.
  if (reused && lost && idempotent(method))
    reply = attempt(method, target, extra_headers, body, reused, lost);
  return reply;
}

}  // namespace fastmm::venues
