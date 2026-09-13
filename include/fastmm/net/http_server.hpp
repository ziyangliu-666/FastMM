#pragma once
// Minimal HTTP/1.1 server for tests and the sim exchange: keep-alive, Content-Length bodies,
// a method+path route table, and WebSocket upgrades on the same port (like Binance, where
// /api/v3/* and /ws share host:443). Not a hot path: std::function handlers and std::string
// responses are fine here.
//
//   accept -> HttpConn (TLS handshake, parse head)
//             ├─ Upgrade: websocket  -> WsServerConnection<Stream> (fd re-registered)
//             └─ route(method, path) -> HttpServerResponse -> keep-alive / close
#include "fastmm/net/byte_stream.hpp"
#include "fastmm/net/http_message.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/recv_buffer.hpp"
#include "fastmm/net/tcp_socket.hpp"
#include "fastmm/net/wire_buffer.hpp"
#include "fastmm/net/ws_server.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fastmm::net {

struct HttpRequest {
  std::string_view method;
  std::string_view target;
  std::string_view path;
  std::string_view query;
  const HttpHeaders* headers = nullptr;
  std::string_view body;

  std::string_view header(std::string_view name) const noexcept { return headers->get(name); }
  std::string_view param(std::string_view name) const noexcept { return query_param(query, name); }
};

struct HttpServerResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;  // extra headers
  bool close = false;                                        // force Connection: close

  // status 0 = swallow the request and send nothing (fault injection: client timeouts).
  static HttpServerResponse none() {
    HttpServerResponse r;
    r.status = 0;
    return r;
  }

  static HttpServerResponse json(int status, std::string body) {
    HttpServerResponse r;
    r.status = status;
    r.body = std::move(body);
    return r;
  }
  static HttpServerResponse text(int status, std::string body) {
    HttpServerResponse r;
    r.status = status;
    r.content_type = "text/plain";
    r.body = std::move(body);
    return r;
  }
};

using HttpRouteHandler = std::function<HttpServerResponse(const HttpRequest&)>;

struct HttpServerConfig {
  std::size_t recv_capacity = 1024 * 1024;  // head + body must fit
  std::size_t send_capacity = 1024 * 1024;
  std::size_t max_body_bytes = 512 * 1024;
  WsServerConfig ws;
};

struct HttpServerStats {
  std::uint64_t accepted = 0;
  std::uint64_t requests = 0;
  std::uint64_t upgrades = 0;
};

constexpr std::string_view http_reason_phrase(int status) noexcept {
  switch (status) {
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 204:
      return "No Content";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 413:
      return "Payload Too Large";
    case 418:
      return "I'm a teapot";
    case 429:
      return "Too Many Requests";
    case 500:
      return "Internal Server Error";
    case 503:
      return "Service Unavailable";
    default:
      return "Unknown";
  }
}

template <ByteStream Stream>
class HttpServer {
 public:
  using StreamFactory = std::function<Stream(TcpSocket&&)>;
  using WsConn = WsServerConnection<Stream>;

  HttpServer(Reactor& reactor, StreamFactory factory, HttpServerConfig cfg = {})
      : reactor_(reactor), factory_(std::move(factory)), cfg_(std::move(cfg)), acceptor_(*this) {}
  ~HttpServer() {
    if (listener_.valid()) reactor_.remove(listener_.fd());
    // Connections detach from the reactor in their destructors.
  }
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  bool listen(const SockAddr& addr, int backlog = 128) {
    listener_ = TcpSocket::listen(addr, backlog);
    if (!listener_.valid()) return false;
    if (!reactor_.add(listener_.fd(), acceptor_, IoEvent::Read)) {
      listener_.close();
      return false;
    }
    if (auto local = listener_.local_addr()) port_ = local->port();
    return true;
  }
  std::uint16_t port() const noexcept { return port_; }

  void route(std::string method, std::string path, HttpRouteHandler handler) {
    routes_[method + " " + path] = std::move(handler);
  }
  void set_default_handler(HttpRouteHandler handler) { default_ = std::move(handler); }
  void set_ws_handler(WsSessionHandler* handler) noexcept { ws_handler_ = handler; }

  std::size_t http_connections() const noexcept { return http_.size(); }
  std::size_t ws_sessions() const noexcept { return ws_.size(); }
  const HttpServerStats& stats() const noexcept { return stats_; }

  template <class Fn>
  void for_each_ws_session(Fn&& fn) {
    for (auto& [id, conn] : ws_) {
      if (conn->is_open()) fn(static_cast<WsSession&>(*conn));
    }
  }
  void broadcast_text(std::string_view text) {
    for_each_ws_session([text](WsSession& s) { s.send_text(text); });
  }

  // Drops every connection (no close frames). Deletion is deferred to the reactor loop so
  // this is safe to call from inside a handler callback.
  void close_all() {
    for (auto& [id, conn] : http_) conn->abort();
    for (auto& [id, conn] : ws_) conn->close_abrupt();
    schedule_reap();
  }

 private:
  // ------------------------------------------------------------------ per-connection state
  class HttpConn final : public IoHandler {
   public:
    HttpConn(HttpServer& server, Stream&& stream, std::uint64_t id)
        : server_(server),
          stream_(std::move(stream)),
          id_(id),
          rx_(server.cfg_.recv_capacity),
          tx_(server.cfg_.send_capacity) {}
    ~HttpConn() override { detach(); }

    bool start() {
      if (!server_.reactor_.add(stream_.fd(), *this, IoEvent::ReadWrite)) return false;
      registered_ = true;
      step();
      return true;
    }
    void abort() noexcept { finish(); }
    bool alive() const noexcept { return alive_; }

    void on_readable() override { step(); }
    void on_writable() override { step(); }
    void on_error(int) override { finish(); }

   private:
    void step() {
      if (!alive_) return;
      if (!ready_) {
        const IoResult h = stream_.handshake();
        if (h.failed()) {
          finish();
          return;
        }
        if (h.would_block()) return;
        ready_ = true;
      }
      flush();
      if (!alive_ || close_after_flush_) return;
      read_requests();
    }

    void read_requests() {
      for (;;) {
        if (!parse_and_dispatch()) return;  // upgraded / closed / need more bytes
        const std::span<std::byte> w = rx_.writable();
        if (w.empty()) {
          respond_and_close(413, "request too large");
          return;
        }
        const IoResult r = stream_.read(w);
        if (r.bytes > 0) rx_.commit(r.bytes);
        if (r.closed || r.failed()) {
          finish();
          return;
        }
        if (r.would_block() && r.bytes == 0) return;
      }
    }

    // Handles every complete request in the buffer. Returns false when the loop must stop
    // (connection gone or upgraded); true when more bytes are needed.
    bool parse_and_dispatch() {
      for (;;) {
        HttpRequestHead head;
        const HttpParseStatus st = parse_request_head(rx_.readable_view(), head);
        if (st == HttpParseStatus::Incomplete) return true;
        if (st != HttpParseStatus::Ok) {
          respond_and_close(400, "malformed request");
          return false;
        }
        std::size_t body_len = 0;
        if (const std::string_view cl = head.headers.get("Content-Length"); !cl.empty()) {
          if (!parse_content_length(cl, body_len) || body_len > server_.cfg_.max_body_bytes) {
            respond_and_close(413, "body too large");
            return false;
          }
        }
        if (rx_.readable_view().size() < head.head_len + body_len) return true;  // wait for body

        ++server_.stats_.requests;
        if (is_upgrade(head)) {
          rx_.consume(head.head_len);
          server_.upgrade(*this, head);  // success: this object is a dead shell; failure: closing
          return false;
        }
        const std::string_view body = rx_.readable_view().substr(head.head_len, body_len);
        HttpRequest req{head.method, head.target, head.path, head.query, &head.headers, body};
        HttpServerResponse resp = server_.dispatch(req);
        const bool close = resp.close || iequals(head.version, "HTTP/1.0") ||
                           header_has_token(head.headers.get("Connection"), "close");
        rx_.consume(head.head_len + body_len);
        if (resp.status == 0) continue;  // deliberately unanswered
        write_response(resp, close);
        if (close) {
          close_after_flush_ = true;
          flush();
          return false;
        }
        if (!alive_) return false;
      }
    }

    static bool is_upgrade(const HttpRequestHead& head) noexcept {
      return iequals(head.headers.get("Upgrade"), "websocket") &&
             header_has_token(head.headers.get("Connection"), "upgrade");
    }

    void write_response(const HttpServerResponse& resp, bool close) {
      std::string head = "HTTP/1.1 " + std::to_string(resp.status) + " " +
                         std::string(http_reason_phrase(resp.status)) +
                         "\r\nContent-Type: " + resp.content_type +
                         "\r\nContent-Length: " + std::to_string(resp.body.size()) +
                         "\r\nConnection: " + (close ? "close" : "keep-alive") + "\r\n";
      for (const auto& [k, v] : resp.headers) head += k + ": " + v + "\r\n";
      head += "\r\n";
      if (!tx_.append(head) || !tx_.append(resp.body)) {
        finish();  // response larger than the send buffer: nothing sensible to do
        return;
      }
      flush();
    }

    void respond_and_close(int status, std::string_view msg) {
      HttpServerResponse r = HttpServerResponse::text(status, std::string(msg));
      write_response(r, /*close=*/true);
      close_after_flush_ = true;
      flush();
    }

    void flush() noexcept {
      if constexpr (requires(Stream& s) { s.flush(); }) {
        const IoResult fl = stream_.flush();
        if (fl.failed()) {
          finish();
          return;
        }
        if (fl.want_write) return;
      }
      while (!tx_.empty()) {
        const IoResult w = stream_.write(tx_.pending());
        tx_.consumed(w.bytes);
        if (w.failed()) {
          finish();
          return;
        }
        if (w.would_block()) return;
      }
      if (close_after_flush_) finish();
    }

    void detach() noexcept {
      if (registered_) {
        server_.reactor_.remove(stream_.fd());
        registered_ = false;
      }
    }
    void finish() noexcept {
      if (!alive_) return;
      alive_ = false;
      detach();
      stream_.close();
      server_.schedule_reap();
    }

    friend class HttpServer;
    HttpServer& server_;
    Stream stream_;
    std::uint64_t id_;
    RecvBuffer rx_;
    WireBuffer tx_;
    bool registered_ = false;
    bool ready_ = false;
    bool alive_ = true;
    bool close_after_flush_ = false;
  };

  class Acceptor final : public IoHandler {
   public:
    explicit Acceptor(HttpServer& s) : server_(s) {}
    void on_readable() override { server_.accept_all(); }
    void on_writable() override {}
    void on_error(int) override {}

   private:
    HttpServer& server_;
  };

  // --------------------------------------------------------------------------- internals
  void accept_all() {
    for (;;) {
      TcpSocket s = listener_.accept();
      if (!s.valid()) return;
      s.set_nodelay(true);
      ++stats_.accepted;
      const std::uint64_t id = next_id_++;
      auto conn = std::make_unique<HttpConn>(*this, factory_(std::move(s)), id);
      HttpConn* raw = conn.get();
      http_.emplace(id, std::move(conn));
      if (!raw->start()) {
        http_.erase(id);
      }
    }
  }

  HttpServerResponse dispatch(const HttpRequest& req) {
    std::string key(req.method);
    key += ' ';
    key += req.path;
    if (auto it = routes_.find(key); it != routes_.end()) return it->second(req);
    if (default_) return default_(req);
    return HttpServerResponse::text(404, "not found");
  }

  // Moves the stream out of `conn` into a WsServerConnection, or answers with an HTTP error
  // (and closes) if the request is not an acceptable upgrade.
  void upgrade(HttpConn& conn, const HttpRequestHead& head) {
    const std::string_view key = head.headers.get("Sec-WebSocket-Key");
    if (head.method != "GET" || key.size() != detail::kWsKeyLen ||
        head.headers.get("Sec-WebSocket-Version") != "13") {
      conn.respond_and_close(400, "bad websocket upgrade");
      return;
    }
    if (ws_handler_ == nullptr || !ws_handler_->accept_upgrade(head.path, head.query)) {
      conn.respond_and_close(404, "no websocket endpoint");
      return;
    }
    ++stats_.upgrades;
    conn.detach();
    const std::uint64_t id = conn.id_;
    auto ws = std::make_unique<WsConn>(reactor_,
                                       std::move(conn.stream_),
                                       *ws_handler_,
                                       id,
                                       std::string(head.target),
                                       key,
                                       cfg_.ws,
                                       [this](WsConn&) { schedule_reap(); });
    conn.alive_ = false;
    schedule_reap();  // the HttpConn shell is garbage now
    WsConn* raw = ws.get();
    ws_.emplace(id, std::move(ws));
    if (!raw->start()) {
      ws_.erase(id);
    }
  }

  void schedule_reap() {
    if (reap_pending_) return;
    reap_pending_ = true;
    reactor_.post([this] { reap(); });
  }
  void reap() {
    reap_pending_ = false;
    for (auto it = http_.begin(); it != http_.end();) {
      it = it->second->alive() ? std::next(it) : http_.erase(it);
    }
    for (auto it = ws_.begin(); it != ws_.end();) {
      it = it->second->is_open() ? std::next(it) : ws_.erase(it);
    }
  }

  Reactor& reactor_;
  StreamFactory factory_;
  HttpServerConfig cfg_;
  Acceptor acceptor_;
  TcpSocket listener_;
  std::uint16_t port_ = 0;
  std::uint64_t next_id_ = 1;
  std::unordered_map<std::string, HttpRouteHandler> routes_;
  HttpRouteHandler default_;
  WsSessionHandler* ws_handler_ = nullptr;
  std::unordered_map<std::uint64_t, std::unique_ptr<HttpConn>> http_;
  std::unordered_map<std::uint64_t, std::unique_ptr<WsConn>> ws_;
  HttpServerStats stats_;
  bool reap_pending_ = false;
};

// A WebSocket-only server is an HttpServer with a session handler and no routes.
template <ByteStream Stream>
using WsServer = HttpServer<Stream>;

}  // namespace fastmm::net
