#pragma once
// Keep-alive HTTP/1.1 client over any ByteStream: one request in flight per connection plus
// a small FIFO, response bodies framed by Content-Length / chunked / connection-close, and a
// per-request reactor timer. Callbacks run on the reactor thread; header and body views are
// valid only for the duration of the callback (they point into the receive buffer).
//
// REST is the venue *control* path (exchangeInfo, listenKey, cancel-all fallback); order
// flow goes over the WebSocket API. Requests are therefore serialised into a std::string at
// enqueue time - simple and safe, not zero-allocation.
#include "fastmm/net/byte_stream.hpp"
#include "fastmm/net/http_message.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/recv_buffer.hpp"
#include "fastmm/net/wire_buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace fastmm::net {

struct HttpResponse {
  int status = 0;                   // 0 when `error` is set
  NetError error = NetError::None;  // Timeout, Closed, Protocol, Canceled, ...
  std::string_view error_detail;
  HttpHeaders headers;     // views into the receive buffer
  std::string_view body;   // decoded (de-chunked) body
  std::int64_t rx_ts = 0;  // reactor clock when the response completed

  std::string_view header(std::string_view name) const noexcept { return headers.get(name); }
  bool ok() const noexcept { return error == NetError::None && status >= 200 && status < 300; }
};

using HttpResponseCallback = std::function<void(const HttpResponse&)>;

struct HttpClientConfig {
  std::size_t recv_capacity = 1024 * 1024;
  std::size_t send_capacity = 256 * 1024;
  std::uint32_t timeout_ms = 5000;  // per request, from the moment it is written
  std::size_t max_queue = 8;        // in-flight + waiting
};

struct HttpClientStats {
  std::uint64_t requests = 0;
  std::uint64_t responses = 0;
  std::uint64_t timeouts = 0;
  std::uint64_t errors = 0;
};

enum class HttpClientState : std::uint8_t { Idle, Connecting, Ready, Closed };

namespace detail {

// Incremental HTTP/1.1 response parser working in place inside a RecvBuffer. Chunked bodies
// are de-chunked by moving chunk data left over the chunk-size lines, so the body ends up
// contiguous right behind the head. Trailers are skipped.
class HttpResponseParser {
 public:
  enum class Result : std::uint8_t { NeedMore, Complete, Invalid };

  void begin(bool head_request) noexcept {
    reset();
    head_request_ = head_request;
  }
  void reset() noexcept;

  // Advances over rx.readable(). On Complete, `out` is filled and `consume_len` is the
  // number of bytes to consume once the callback returns.
  Result parse(RecvBuffer& rx, HttpResponse& out, std::size_t& consume_len) noexcept;
  // The peer closed: completes a close-delimited body, otherwise reports truncation.
  Result on_eof(RecvBuffer& rx, HttpResponse& out, std::size_t& consume_len) noexcept;

  bool connection_close() const noexcept { return connection_close_; }
  const char* error() const noexcept { return error_; }

 private:
  enum class Stage : std::uint8_t {
    Head,
    FixedBody,
    ChunkSize,
    ChunkData,
    ChunkEnd,
    Trailers,
    UntilClose
  };

  Result parse_head(RecvBuffer& rx) noexcept;
  Result finish(RecvBuffer& rx, HttpResponse& out, std::size_t& consume_len) noexcept;
  Result fail(const char* why) noexcept {
    error_ = why;
    return Result::Invalid;
  }

  Stage stage_ = Stage::Head;
  bool head_request_ = false;
  bool connection_close_ = false;
  std::size_t head_len_ = 0;
  std::size_t body_len_ = 0;  // decoded body bytes at [head_len_, head_len_ + body_len_)
  std::size_t scan_off_ = 0;  // next unparsed byte (relative to readable())
  std::size_t chunk_remaining_ = 0;
  std::size_t expected_len_ = 0;  // Content-Length
  const char* error_ = "";
};

}  // namespace detail

template <ByteStream Stream>
class HttpClient final : public IoHandler {
 public:
  HttpClient(Reactor& reactor,
             Stream&& stream,
             std::string host,
             std::uint16_t port,
             bool tls,
             const HttpClientConfig& cfg = {})
      : reactor_(reactor),
        stream_(std::move(stream)),
        host_(std::move(host)),
        port_(port),
        tls_(tls),
        cfg_(cfg),
        rx_(cfg.recv_capacity),
        tx_(cfg.send_capacity) {}
  ~HttpClient() override { close(); }
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  bool start() {
    if (state_ != HttpClientState::Idle) return false;
    if (!reactor_.add(stream_.fd(), *this, IoEvent::ReadWrite)) return false;
    registered_ = true;
    state_ = HttpClientState::Connecting;
    drive_connect();
    return true;
  }

  // Queues a request. `extra_headers` is a pre-formatted "Name: value\r\n" block. Returns
  // false if the client is closed or the queue is full (callback not invoked).
  bool request(std::string_view method,
               std::string_view target,
               std::string_view extra_headers,
               std::string_view body,
               HttpResponseCallback cb) {
    if (state_ == HttpClientState::Closed || queue_.size() >= cfg_.max_queue) return false;
    Pending p;
    p.head_request = method == "HEAD";
    p.cb = std::move(cb);
    p.bytes.reserve(method.size() + target.size() + host_.size() + extra_headers.size() +
                    body.size() + 64);
    p.bytes.append(method).append(" ").append(target).append(" HTTP/1.1\r\nHost: ").append(host_);
    if (port_ != 0 && port_ != (tls_ ? 443 : 80)) p.bytes.append(":").append(std::to_string(port_));
    p.bytes.append("\r\n").append(extra_headers);
    if (!body.empty() || method == "POST" || method == "PUT" || method == "PATCH") {
      p.bytes.append("Content-Length: ").append(std::to_string(body.size())).append("\r\n");
    }
    p.bytes.append("\r\n").append(body);
    queue_.push_back(std::move(p));
    ++stats_.requests;
    if (state_ == HttpClientState::Ready) send_next();
    return true;
  }

  // Tears the connection down; every queued/in-flight request gets NetError::Canceled.
  void close() noexcept { fail_all(NetError::Canceled, "closed"); }

  HttpClientState state() const noexcept { return state_; }
  bool is_ready() const noexcept { return state_ == HttpClientState::Ready; }
  bool in_flight() const noexcept { return in_flight_; }
  std::size_t queued() const noexcept { return queue_.size(); }
  const HttpClientStats& stats() const noexcept { return stats_; }
  Stream& stream() noexcept { return stream_; }

  // --- IoHandler -------------------------------------------------------------------------
  void on_readable() override {
    if (state_ == HttpClientState::Connecting) {
      drive_connect();
    } else if (state_ == HttpClientState::Ready) {
      read_responses();
    }
  }
  void on_writable() override {
    if (state_ == HttpClientState::Connecting) {
      drive_connect();
    } else if (state_ == HttpClientState::Ready) {
      flush();
    }
  }
  void on_error(int) override { fail_all(NetError::Syscall, "socket error"); }

 private:
  struct Pending {
    std::string bytes;
    HttpResponseCallback cb;
    bool head_request = false;
  };

  void drive_connect() {
    const IoResult r = stream_.handshake();
    if (r.failed()) {
      fail_all(r.error, r.error == NetError::Tls ? "tls handshake failed" : "connect failed");
      return;
    }
    if (r.would_block()) return;
    state_ = HttpClientState::Ready;
    send_next();
  }

  void send_next() {
    if (in_flight_ || queue_.empty() || state_ != HttpClientState::Ready) return;
    Pending& p = queue_.front();
    if (!tx_.append(p.bytes)) {
      // Larger than the whole send buffer: fail just this request.
      Pending failed = std::move(queue_.front());
      queue_.pop_front();
      deliver_error(failed, NetError::Overflow, "request exceeds send buffer");
      send_next();
      return;
    }
    in_flight_ = true;
    parser_.begin(p.head_request);
    timer_ = reactor_.add_timer_after(static_cast<std::int64_t>(cfg_.timeout_ms) * 1'000'000,
                                      [this] { on_timeout(); });
    flush();
  }

  void on_timeout() {
    timer_ = kInvalidTimer;
    ++stats_.timeouts;
    // The response boundary is now unknown; the connection cannot be reused.
    fail_all(NetError::Timeout, "request timed out", /*first_only_error=*/true);
  }

  void flush() noexcept {
    if constexpr (requires(Stream& s) { s.flush(); }) {
      const IoResult fl = stream_.flush();
      if (fl.failed()) {
        fail_all(fl.error, "flush failed");
        return;
      }
      if (fl.want_write) return;
    }
    while (!tx_.empty()) {
      const IoResult w = stream_.write(tx_.pending());
      tx_.consumed(w.bytes);
      if (w.failed()) {
        fail_all(w.error, "write failed");
        return;
      }
      if (w.would_block()) return;
    }
  }

  void read_responses() {
    for (;;) {
      const std::span<std::byte> w = rx_.writable();
      if (w.empty()) {
        fail_all(NetError::Overflow, "response exceeds receive buffer");
        return;
      }
      const IoResult r = stream_.read(w);
      if (r.bytes > 0) {
        rx_.commit(r.bytes);
        if (!process()) return;
      }
      if (r.closed) {
        on_eof();
        return;
      }
      if (r.failed()) {
        fail_all(r.error, "read failed");
        return;
      }
      if (r.would_block()) return;
    }
  }

  // Parses the in-flight response; returns false if the connection was torn down.
  bool process() {
    if (!in_flight_) {
      // Bytes without a request in flight: the server is misbehaving.
      if (!rx_.empty()) fail_all(NetError::Protocol, "unsolicited response bytes");
      return false;
    }
    HttpResponse resp;
    std::size_t consume = 0;
    const auto res = parser_.parse(rx_, resp, consume);
    if (res == detail::HttpResponseParser::Result::NeedMore) return true;
    if (res == detail::HttpResponseParser::Result::Invalid) {
      fail_all(NetError::Protocol, parser_.error());
      return false;
    }
    complete(resp, consume);
    return state_ == HttpClientState::Ready;
  }

  void on_eof() {
    if (in_flight_) {
      HttpResponse resp;
      std::size_t consume = 0;
      if (parser_.on_eof(rx_, resp, consume) == detail::HttpResponseParser::Result::Complete) {
        complete(resp, consume);
      }
    }
    fail_all(NetError::Closed, "connection closed");
  }

  void complete(HttpResponse& resp, std::size_t consume) {
    cancel_timer();
    in_flight_ = false;
    ++stats_.responses;
    resp.rx_ts = Reactor::now_ns();
    Pending done = std::move(queue_.front());
    queue_.pop_front();
    const bool close_after = parser_.connection_close();
    done.cb(resp);
    rx_.consume(consume);
    if (close_after) {
      fail_all(NetError::Closed, "server closed the connection");
      return;
    }
    if (state_ == HttpClientState::Ready) send_next();
  }

  void cancel_timer() noexcept {
    if (timer_ != kInvalidTimer) {
      reactor_.cancel_timer(timer_);
      timer_ = kInvalidTimer;
    }
  }

  void deliver_error(Pending& p, NetError err, const char* detail) {
    HttpResponse resp;
    resp.error = err;
    resp.error_detail = detail;
    ++stats_.errors;
    if (p.cb) p.cb(resp);
  }

  // Closes the stream and fails every pending request. With first_only_error, the
  // in-flight request receives `err` and the rest are Canceled.
  void fail_all(NetError err, const char* detail, bool first_only_error = false) noexcept {
    if (state_ == HttpClientState::Closed) return;
    state_ = HttpClientState::Closed;
    cancel_timer();
    if (registered_) {
      reactor_.remove(stream_.fd());
      registered_ = false;
    }
    stream_.close();
    in_flight_ = false;
    std::deque<Pending> pending;
    pending.swap(queue_);
    bool first = true;
    for (Pending& p : pending) {
      deliver_error(p, (first || !first_only_error) ? err : NetError::Canceled, detail);
      first = false;
    }
  }

  Reactor& reactor_;
  Stream stream_;
  std::string host_;
  std::uint16_t port_;
  bool tls_;
  HttpClientConfig cfg_;
  RecvBuffer rx_;
  WireBuffer tx_;
  detail::HttpResponseParser parser_;
  std::deque<Pending> queue_;
  HttpClientStats stats_;
  HttpClientState state_ = HttpClientState::Idle;
  TimerId timer_ = kInvalidTimer;
  bool registered_ = false;
  bool in_flight_ = false;
};

}  // namespace fastmm::net
