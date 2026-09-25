#pragma once
// RFC 6455 WebSocket client over any ByteStream, driven by a Reactor.
//
// Lifecycle: start() registers the fd and drives Stream::handshake() (TCP connect, TLS),
// then sends the HTTP upgrade, verifies Sec-WebSocket-Accept, and switches to framing.
// Receive path: one read() into the RecvBuffer, then WsMessageAssembler delivers complete
// messages in place (no copies for unfragmented frames; text payloads keep kPadding bytes
// behind them for simdjson). Send path: frames are encoded + masked into a WireBuffer and
// written immediately; leftovers are flushed on EPOLLOUT.
//
// Extensions: none are negotiated. permessage-deflate would be requested in
// detail::ws_build_upgrade_request and accepted in ws_check_upgrade_response; frames with
// RSV1 set are rejected until then (see WsMessageAssembler).
#include "fastmm/net/byte_stream.hpp"
#include "fastmm/net/http_message.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/recv_buffer.hpp"
#include "fastmm/net/wire_buffer.hpp"
#include "fastmm/net/ws_frame.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace fastmm::net {

// Milestones before the upgrade completes, so owners can report fine-grained states.
enum class WsProgress : std::uint8_t {
  TcpConnected,  // TCP established (TLS handshake starts next for TLS streams)
  UpgradeSent,   // transport ready, HTTP upgrade request written
};

template <class H>
concept WsClientHandler = requires(H& h,
                                   std::string_view s,
                                   std::span<const std::byte> b,
                                   std::int64_t ts,
                                   std::uint16_t code,
                                   NetError e,
                                   WsProgress p) {
  h.on_ws_progress(p);
  h.on_ws_open();
  h.on_ws_text(s, ts);
  h.on_ws_binary(b, ts);
  h.on_ws_ping(b);
  h.on_ws_pong(b);
  h.on_ws_close(code, s);  // peer sent a close frame (or replied to ours)
  h.on_ws_error(e, s);     // transport/protocol failure; the stream is closed
};

struct WsClientConfig {
  std::size_t recv_capacity =
      std::size_t{4} * 1024 * 1024;                      // RecvBuffer; bounds the largest message
  std::size_t send_capacity = std::size_t{1024} * 1024;  // WireBuffer for outgoing frames
  std::size_t max_message_bytes = 0;                     // 0 = recv_capacity - kWsMaxHeaderSize
  bool validate_utf8 = true;  // reject text messages that are not UTF-8 (close code 1007)
};

struct WsStats {
  std::uint64_t frames_rx = 0;
  std::uint64_t frames_tx = 0;
  std::uint64_t bytes_rx = 0;
  std::uint64_t bytes_tx = 0;
  std::uint64_t drops = 0;  // sends refused because the send buffer was full
};

enum class WsState : std::uint8_t { Idle, Connecting, Upgrading, Open, Closing, Closed };

namespace detail {

inline constexpr std::size_t kWsKeyLen = 24;     // base64 of 16 random bytes
inline constexpr std::size_t kWsAcceptLen = 28;  // base64 of SHA-1

// base64(SHA1(client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")).
void ws_compute_accept_key(std::string_view client_key, std::span<char, kWsAcceptLen> out) noexcept;
void ws_generate_client_key(std::span<char, kWsKeyLen> out) noexcept;

// Appends the upgrade request. `extra_headers` is a pre-formatted "Name: value\r\n" block
// (venue-specific headers such as API keys). Returns false if the buffer is too small.
bool ws_build_upgrade_request(WireBuffer& out,
                              std::string_view host,
                              std::uint16_t port,
                              bool tls,
                              std::string_view target,
                              std::string_view key,
                              std::string_view extra_headers) noexcept;

// nullptr when the response is a valid upgrade, else a static description of the failure.
const char* ws_check_upgrade_response(const HttpResponseHead& head,
                                      std::string_view expected_accept) noexcept;

// xorshift64* for per-frame masks: RFC 6455 wants unpredictability against proxies, not
// cryptographic strength; seeded from RAND_bytes at construction.
class MaskGenerator {
 public:
  MaskGenerator() noexcept;
  void next(std::uint8_t out[4]) noexcept {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    const std::uint64_t v = state_ * 0x2545F4914F6CDD1DULL;
    const auto w = static_cast<std::uint32_t>(v >> 32);
    out[0] = static_cast<std::uint8_t>(w);
    out[1] = static_cast<std::uint8_t>(w >> 8);
    out[2] = static_cast<std::uint8_t>(w >> 16);
    out[3] = static_cast<std::uint8_t>(w >> 24);
  }

 private:
  std::uint64_t state_;
};

}  // namespace detail

template <ByteStream Stream, WsClientHandler Handler>
class WsClient final : public IoHandler {
 public:
  WsClient(Reactor& reactor, Stream&& stream, Handler& handler, const WsClientConfig& cfg = {})
      : reactor_(reactor),
        stream_(std::move(stream)),
        handler_(handler),
        rx_(cfg.recv_capacity),
        tx_(cfg.send_capacity),
        assembler_(rx_,
                   cfg.max_message_bytes != 0 ? cfg.max_message_bytes
                                              : cfg.recv_capacity - kWsMaxHeaderSize,
                   /*require_masked=*/false,
                   cfg.validate_utf8) {}

  ~WsClient() override { detach(); }
  WsClient(const WsClient&) = delete;
  WsClient& operator=(const WsClient&) = delete;

  // Registers with the reactor and begins the connect/TLS/upgrade sequence. The stream must
  // already hold a socket that is connected or connecting.
  bool start(std::string_view host,
             std::uint16_t port,
             bool tls,
             std::string_view target,
             std::string_view extra_headers = {}) {
    if (state_ != WsState::Idle) return false;
    host_ = std::string(host);
    port_ = port;
    tls_ = tls;
    target_ = std::string(target);
    extra_headers_ = std::string(extra_headers);
    if (!reactor_.add(stream_.fd(), *this, IoEvent::ReadWrite)) return false;
    registered_ = true;
    state_ = WsState::Connecting;
    drive_connect();
    return true;
  }

  // --- sending (client frames are always masked) ---------------------------------------
  // False means the frame did not go out: the client is not open, the frame did not fit, or the
  // write failed and closed the stream (on_ws_error already fired). A frame the kernel did not
  // take yet is queued for writability and counts as sent. Between cork() and uncork() a data
  // frame is only encoded, so uncork() reports its write instead.
  bool send_text(std::string_view text) noexcept {
    return send_frame(
        WsOpcode::Text,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
  }
  bool send_binary(std::span<const std::byte> data) noexcept {
    return send_frame(WsOpcode::Binary, data);
  }
  bool send_ping(std::span<const std::byte> data = {}) noexcept {
    return send_frame(WsOpcode::Ping, data);
  }
  bool send_pong(std::span<const std::byte> data = {}) noexcept {
    return send_frame(WsOpcode::Pong, data);
  }

  // Between cork() and uncork() data frames are only encoded into the send buffer; uncork()
  // writes them together (one TLS write and one write(2) for the lot when the kernel takes it).
  // uncork() may invoke on_ws_error, like send_text(); it returns false when the client is not
  // open afterwards.
  void cork() noexcept { corked_ = true; }
  bool uncork() noexcept {
    corked_ = false;
    if (state_ != WsState::Open) return false;
    flush();
    return state_ == WsState::Open;
  }

  // Starts the closing handshake; on_ws_close fires when the peer echoes the close frame.
  bool send_close(WsCloseCode code = WsCloseCode::Normal, std::string_view reason = {}) noexcept {
    if (state_ != WsState::Open) return false;
    std::array<std::byte, kWsMaxControlPayload> payload{};
    const std::size_t n =
        ws_encode_close_payload(payload, static_cast<std::uint16_t>(code), reason);
    if (!encode_frame(WsOpcode::Close, std::span<const std::byte>(payload.data(), n))) return false;
    state_ = WsState::Closing;  // before flush(): a write failure may report on_ws_error
    flush();
    return true;
  }

  // Immediate teardown, no callbacks.
  void close() noexcept {
    detach();
    stream_.close();
    state_ = WsState::Closed;
  }

  WsState state() const noexcept { return state_; }
  bool is_open() const noexcept { return state_ == WsState::Open; }
  const WsStats& stats() const noexcept { return stats_; }
  std::size_t pending_send_bytes() const noexcept { return tx_.size(); }
  Stream& stream() noexcept { return stream_; }
  int fd() const noexcept { return stream_.fd(); }

  // --- IoHandler -------------------------------------------------------------------------
  void on_readable() override {
    switch (state_) {
      case WsState::Connecting:
        drive_connect();
        break;
      case WsState::Upgrading:
        read_upgrade_response();
        break;
      case WsState::Open:
      case WsState::Closing:
        read_frames();
        break;
      case WsState::Idle:
      case WsState::Closed:
        break;
    }
  }
  void on_writable() override {
    if (state_ == WsState::Connecting) {
      drive_connect();
    } else if (state_ != WsState::Idle && state_ != WsState::Closed) {
      flush();
    }
  }
  void on_error(int err) override {
    fail(NetError::Syscall, err != 0 ? "socket error" : "epoll error");
  }

 private:
  void detach() noexcept {
    if (registered_) {
      reactor_.remove(stream_.fd());
      registered_ = false;
    }
  }

  void drive_connect() {
    const IoResult r = stream_.handshake();
    if (r.failed()) {
      fail(r.error, r.error == NetError::Tls ? "tls handshake failed" : "connect failed");
      return;
    }
    if (!tcp_reported_ && (!r.would_block() || tcp_connected())) {
      tcp_reported_ = true;
      handler_.on_ws_progress(WsProgress::TcpConnected);
      if (state_ != WsState::Connecting) return;  // handler closed us
    }
    if (r.would_block()) return;
    // Transport ready: send the upgrade request.
    detail::ws_generate_client_key(std::span<char, detail::kWsKeyLen>(key_.data(), key_.size()));
    detail::ws_compute_accept_key(
        std::string_view(key_.data(), key_.size()),
        std::span<char, detail::kWsAcceptLen>(expected_accept_.data(), expected_accept_.size()));
    if (!detail::ws_build_upgrade_request(tx_,
                                          host_,
                                          port_,
                                          tls_,
                                          target_,
                                          std::string_view(key_.data(), key_.size()),
                                          extra_headers_)) {
      fail(NetError::Overflow, "upgrade request exceeds send buffer");
      return;
    }
    state_ = WsState::Upgrading;
    flush();
    if (state_ == WsState::Upgrading) handler_.on_ws_progress(WsProgress::UpgradeSent);
    // The server may already have answered (loopback); readable events will follow anyway.
  }

  // TLS streams expose whether the underlying TCP connect finished; plain streams are
  // connected exactly when their handshake() succeeds.
  bool tcp_connected() const noexcept {
    if constexpr (requires(const Stream& s) { s.transport_ready(); }) {
      return stream_.transport_ready();
    } else {
      return false;
    }
  }

  void read_upgrade_response() {
    for (;;) {
      const std::span<std::byte> w = rx_.writable();
      if (w.empty()) {
        fail(NetError::Overflow, "upgrade response exceeds receive buffer");
        return;
      }
      const IoResult r = stream_.read(w);
      if (r.bytes > 0) {
        rx_.commit(r.bytes);
        stats_.bytes_rx += r.bytes;
        HttpResponseHead head;
        const HttpParseStatus st = parse_response_head(rx_.readable_view(), head);
        if (st == HttpParseStatus::Invalid || st == HttpParseStatus::TooManyHeaders) {
          fail(NetError::Protocol, "malformed upgrade response");
          return;
        }
        if (st == HttpParseStatus::Ok) {
          if (const char* why = detail::ws_check_upgrade_response(
                  head, std::string_view(expected_accept_.data(), expected_accept_.size()))) {
            fail(NetError::Protocol, why);
            return;
          }
          rx_.consume(head.head_len);
          state_ = WsState::Open;
          handler_.on_ws_open();
          if (state_ != WsState::Open && state_ != WsState::Closing) return;  // handler closed us
          rx_ts_ = Reactor::now_ns();
          if (!process_frames()) return;
          read_frames();  // drain anything else already queued (edge-triggered)
          return;
        }
      }
      if (r.closed) {
        fail(NetError::Closed, "eof during upgrade");
        return;
      }
      if (r.failed()) {
        fail(r.error, "read failed during upgrade");
        return;
      }
      if (r.would_block()) return;
    }
  }

  // Edge-triggered drain: read until the stream wants more.
  void read_frames() {
    for (;;) {
      const std::span<std::byte> w = rx_.writable();
      if (w.empty()) {
        fail_close(WsCloseCode::MessageTooBig, "message larger than receive buffer");
        return;
      }
      const IoResult r = stream_.read(w);
      if (r.bytes > 0) {
        rx_.commit(r.bytes);
        stats_.bytes_rx += r.bytes;
        rx_ts_ = Reactor::now_ns();
        if (!process_frames()) return;
        if (state_ == WsState::Closed) return;
        if (input_drained()) return;  // skip the read that would return EAGAIN
      }
      if (r.closed) {
        fail(NetError::Closed, "eof");
        return;
      }
      if (r.failed()) {
        fail(r.error, "read failed");
        return;
      }
      if (r.would_block()) return;
    }
  }

  // The socket had nothing more after the last read and the event reported no hang-up: data that
  // arrives later comes with a new readable event (edge-triggered contract).
  bool input_drained() const noexcept {
    if constexpr (requires(const Stream& s) {
                    { s.input_drained() } -> std::same_as<bool>;
                  }) {
      return !reactor_.event_hangup() && stream_.input_drained();
    } else {
      return false;
    }
  }

  // Returns false if the connection was torn down.
  bool process_frames() {
    const detail::WsAssembleError err = assembler_.process(*this);
    if (err) {
      fail_close(err.code, err.detail);
      return false;
    }
    return state_ != WsState::Closed;
  }

  // WsMessageAssembler sink.
  friend class detail::WsMessageAssembler;

 public:
  void on_message(WsOpcode op, std::span<std::byte> payload) noexcept {
    if (state_ == WsState::Closed) return;  // handler closed us mid-batch
    ++stats_.frames_rx;
    if (op == WsOpcode::Text) {
      handler_.on_ws_text(
          std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()), rx_ts_);
    } else {
      handler_.on_ws_binary(payload, rx_ts_);
    }
  }
  void on_control(WsOpcode op, std::span<std::byte> payload) noexcept {
    if (state_ == WsState::Closed) return;
    ++stats_.frames_rx;
    switch (op) {
      case WsOpcode::Ping:
        handler_.on_ws_ping(payload);
        if (state_ == WsState::Open) send_pong(payload);
        break;
      case WsOpcode::Pong:
        handler_.on_ws_pong(payload);
        break;
      case WsOpcode::Close: {
        const std::uint16_t code = ws_decode_close_code(payload);
        const std::string_view reason =
            payload.size() > 2 ? std::string_view(reinterpret_cast<const char*>(payload.data()) + 2,
                                                  payload.size() - 2)
                               : std::string_view{};
        if (state_ == WsState::Open) {
          // Peer-initiated close: echo the code (§5.5.1) then tear down.
          std::array<std::byte, kWsMaxControlPayload> echo{};
          const std::size_t n = ws_encode_close_payload(
              echo, code == static_cast<std::uint16_t>(WsCloseCode::NoStatus) ? 1000 : code, {});
          send_frame(WsOpcode::Close, std::span<const std::byte>(echo.data(), n));
          if (state_ == WsState::Closed) break;  // the echo failed: on_ws_error already fired
        }
        close();
        handler_.on_ws_close(code, reason);
        break;
      }
      default:
        break;
    }
  }

 private:
  bool encode_frame(WsOpcode op, std::span<const std::byte> payload) noexcept {
    std::uint8_t mask[4];
    masks_.next(mask);
    if (!ws_encode_frame(tx_, op, /*fin=*/true, payload, mask)) {
      ++stats_.drops;
      return false;
    }
    ++stats_.frames_tx;
    return true;
  }

  // Encodes + writes one masked frame. May invoke on_ws_error (via flush) before returning;
  // callers must not touch members afterwards unless they re-check state_.
  bool send_frame(WsOpcode op, std::span<const std::byte> payload) noexcept {
    if (state_ != WsState::Open) return false;
    if (corked_ && op != WsOpcode::Text && op != WsOpcode::Binary) corked_ = false;
    if (corked_ && tx_.free_space() < payload.size() + kWsMaxHeaderSize) {
      flush();  // full while corked: write what is queued first
      if (state_ != WsState::Open) return false;
    }
    if (!encode_frame(op, payload)) return false;
    if (corked_) return true;  // queued; uncork() reports the write
    flush();
    return state_ == WsState::Open;  // a failed write closed the stream: the frame never left
  }

  void flush() noexcept {
    if constexpr (requires(Stream& s) { s.flush(); }) {
      const IoResult fl = stream_.flush();  // TLS: push buffered ciphertext first
      if (fl.failed()) {
        fail(fl.error, "flush failed");
        return;
      }
      if (fl.want_write) return;
    }
    while (!tx_.empty()) {
      const IoResult w = stream_.write(tx_.pending());
      tx_.consumed(w.bytes);
      stats_.bytes_tx += w.bytes;
      if (w.failed()) {
        fail(w.error, "write failed");
        return;
      }
      if (w.would_block()) return;
    }
  }

  // Protocol violation: send a close frame with `code`, then tear down and report.
  void fail_close(WsCloseCode code, const char* detail) noexcept {
    if (state_ == WsState::Open) {
      std::array<std::byte, kWsMaxControlPayload> payload{};
      const std::size_t n = ws_encode_close_payload(payload, static_cast<std::uint16_t>(code), {});
      send_frame(WsOpcode::Close, std::span<const std::byte>(payload.data(), n));
    }
    fail(code == WsCloseCode::MessageTooBig ? NetError::Overflow : NetError::Protocol, detail);
  }

  void fail(NetError err, const char* detail) noexcept {
    if (state_ == WsState::Closed) return;
    close();
    handler_.on_ws_error(err, detail);
  }

  Reactor& reactor_;
  Stream stream_;
  Handler& handler_;
  RecvBuffer rx_;
  WireBuffer tx_;
  detail::WsMessageAssembler assembler_;
  detail::MaskGenerator masks_;
  WsStats stats_;
  WsState state_ = WsState::Idle;
  bool registered_ = false;
  bool corked_ = false;
  bool tcp_reported_ = false;
  bool tls_ = false;
  std::uint16_t port_ = 0;
  std::int64_t rx_ts_ = 0;
  std::array<char, detail::kWsKeyLen> key_{};
  std::array<char, detail::kWsAcceptLen> expected_accept_{};
  std::string host_;
  std::string target_;
  std::string extra_headers_;
};

}  // namespace fastmm::net
