#include "fastmm/venues/blocking_http.hpp"

#include "fastmm/net/dns.hpp"
#include "fastmm/net/http_client.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/url.hpp"

#include <stdexcept>

namespace fastmm::venues {

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

namespace {

template <class Stream>
HttpReply run_request(net::Reactor& reactor,
                      Stream stream,  // by value: the client takes ownership of the socket
                      const std::string& host,
                      std::uint16_t port,
                      bool tls,
                      const BlockingHttpOptions& opts,
                      std::string_view method,
                      std::string_view target,
                      std::string_view extra_headers,
                      std::string_view body) {
  HttpReply reply;
  net::HttpClientConfig cfg;
  cfg.timeout_ms = opts.timeout_ms;
  net::HttpClient<Stream> client(reactor, std::move(stream), host, port, tls, cfg);
  if (!client.start()) {
    reply.error = "cannot register socket";
    return reply;
  }
  bool done = false;
  const bool queued =
      client.request(method, target, extra_headers, body, [&](const net::HttpResponse& r) {
        done = true;
        if (r.error != net::NetError::None) {
          reply.error = std::string(net::to_string(r.error)) + ": " + std::string(r.error_detail);
          return;
        }
        reply.status = r.status;
        reply.body = std::string(r.body);
        for (const net::HttpHeader& h : r.headers.view())
          reply.headers.emplace_back(std::string(h.name), std::string(h.value));
      });
  if (!queued) {
    // start() drives the non-blocking connect; on loopback a refused connection fails at
    // once and closes the client before the request can be queued.
    reply.error = client.state() == net::HttpClientState::Closed
                      ? "connect failed (connection refused or unreachable)"
                      : "request not queued";
    return reply;
  }
  const std::int64_t deadline =
      net::Reactor::now_ns() +
      static_cast<std::int64_t>(opts.timeout_ms + opts.connect_timeout_ms) * 1'000'000;
  // `done` is set by the response callback, which run_once() invokes from inside the loop.
  while (!done) {  // NOLINT(bugprone-infinite-loop)
    if (net::Reactor::now_ns() > deadline) {
      reply.error = "timeout";
      break;
    }
    reactor.run_once(50);
  }
  return reply;
}

}  // namespace

HttpReply BlockingHttp::request(std::string_view method,
                                std::string_view target,
                                std::string_view extra_headers,
                                std::string_view body) {
  HttpReply reply;
  const net::ResolveResult res = net::resolve_sync(host_, port_);
  if (!res.ok()) {
    reply.error = "resolve failed: " + res.error_text();
    return reply;
  }
  net::TcpSocket sock;
  if (sock.connect(res.addrs.front()) == net::ConnectStatus::Error) {
    reply.error = "connect failed";
    return reply;
  }
  sock.set_nodelay(true);
  const std::string full_target = base_path_ + std::string(target);
  net::Reactor reactor;
  if (tls_) {
    net::TlsContext ctx;
    if (!opts_.ca_file.empty()) ctx.set_ca_file(opts_.ca_file);
    ctx.set_insecure(opts_.insecure_tls);
    return run_request(
        reactor,
        net::TlsStream<net::PlainStream>(ctx, net::PlainStream(std::move(sock)), host_),
        host_,
        port_,
        true,
        opts_,
        method,
        full_target,
        extra_headers,
        body);
  }
  return run_request(reactor,
                     net::PlainStream(std::move(sock)),
                     host_,
                     port_,
                     false,
                     opts_,
                     method,
                     full_target,
                     extra_headers,
                     body);
}

}  // namespace fastmm::venues
