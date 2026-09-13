#pragma once
// Server side of RFC 6455: a WsServerConnection is created by HttpServer once it has
// validated an "Upgrade: websocket" request. It sends the 101 response, then runs the same
// in-place frame assembler as the client with the masking rules reversed (client frames
// must be masked, server frames are sent unmasked). Used by tests and the sim exchange;
// callbacks go through the virtual WsSessionHandler (control-path quality is fine here).
#include "fastmm/net/byte_stream.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/recv_buffer.hpp"
#include "fastmm/net/wire_buffer.hpp"
#include "fastmm/net/ws_client.hpp"
#include "fastmm/net/ws_frame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace fastmm::net {

// What a handler may do with a live session.
class WsSession {
 public:
  virtual ~WsSession() = default;
  virtual bool send_text(std::string_view text) noexcept = 0;
  virtual bool send_binary(std::span<const std::byte> data) noexcept = 0;
  virtual bool send_ping(std::span<const std::byte> data = {}) noexcept = 0;
  virtual bool send_close(WsCloseCode code = WsCloseCode::Normal,
                          std::string_view reason = {}) noexcept = 0;
  // Sends one frame verbatim (fragments, control frames): tests and fault injection.
  virtual bool send_raw(WsOpcode op, bool fin, std::span<const std::byte> payload) noexcept = 0;
  // Drops the TCP connection without a close frame (fault injection).
  virtual void close_abrupt() noexcept = 0;
  virtual std::uint64_t id() const noexcept = 0;
  virtual std::string_view path() const noexcept = 0;
  virtual std::string_view query() const noexcept = 0;
  virtual bool is_open() const noexcept = 0;
  virtual const WsStats& stats() const noexcept = 0;
};

class WsSessionHandler {
 public:
  virtual ~WsSessionHandler() = default;
  // Return false to answer the upgrade request with 404 instead of 101.
  virtual bool accept_upgrade(std::string_view /*path*/, std::string_view /*query*/) {
    return true;
  }
  virtual void on_open(WsSession&) {}
  virtual void on_text(WsSession&, std::string_view) {}
  virtual void on_binary(WsSession&, std::span<const std::byte>) {}
  virtual void on_ping(WsSession&, std::span<const std::byte>) {}  // pong is sent automatically
  virtual void on_pong(WsSession&, std::span<const std::byte>) {}
  // Close frame received from the peer, or our close handshake completed.
  virtual void on_close(WsSession&, std::uint16_t /*code*/, std::string_view /*reason*/) {}
  virtual void on_error(WsSession&, NetError, std::string_view /*detail*/) {}
};

struct WsServerConfig {
  std::size_t recv_capacity = 1024 * 1024;
  std::size_t send_capacity = 4 * 1024 * 1024;  // market-data fan-out bursts
  std::size_t max_message_bytes = 0;            // 0 = recv_capacity - kWsMaxHeaderSize
};

template <ByteStream Stream>
class WsServerConnection final : public IoHandler, public WsSession {
 public:
  using ClosedFn = std::function<void(WsServerConnection&)>;

  WsServerConnection(Reactor& reactor,
                     Stream&& stream,
                     WsSessionHandler& handler,
                     std::uint64_t id,
                     std::string target,
                     std::string_view client_key,
                     const WsServerConfig& cfg,
                     ClosedFn on_closed)
      : reactor_(reactor),
        stream_(std::move(stream)),
        handler_(handler),
        id_(id),
        target_(std::move(target)),
        rx_(cfg.recv_capacity),
        tx_(cfg.send_capacity),
        assembler_(rx_,
                   cfg.max_message_bytes != 0 ? cfg.max_message_bytes
                                              : cfg.recv_capacity - kWsMaxHeaderSize,
                   /*require_masked=*/true),
        on_closed_(std::move(on_closed)) {
    detail::ws_compute_accept_key(
        client_key, std::span<char, detail::kWsAcceptLen>(accept_.data(), accept_.size()));
    const std::size_t q = target_.find('?');
    path_len_ = q == std::string::npos ? target_.size() : q;
  }
  ~WsServerConnection() override { detach(); }

  // Registers the fd, sends "101 Switching Protocols" and reports on_open.
  bool start() {
    if (!reactor_.add(stream_.fd(), *this, IoEvent::ReadWrite)) return false;
    registered_ = true;
    tx_.append(
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
        "Upgrade\r\nSec-WebSocket-Accept: ");
    tx_.append(std::string_view(accept_.data(), accept_.size()));
    tx_.append("\r\n\r\n");
    open_ = true;
    flush();
    if (!open_) return false;
    handler_.on_open(*this);
    if (open_) read_frames();  // frames may already be queued behind the upgrade request
    return true;
  }

  // --- WsSession ---------------------------------------------------------------------------
  bool send_text(std::string_view text) noexcept override {
    return send_frame(
        WsOpcode::Text,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
  }
  bool send_binary(std::span<const std::byte> data) noexcept override {
    return send_frame(WsOpcode::Binary, data);
  }
  bool send_ping(std::span<const std::byte> data) noexcept override {
    return send_frame(WsOpcode::Ping, data);
  }
  bool send_close(WsCloseCode code, std::string_view reason) noexcept override {
    if (!open_ || closing_) return false;
    std::array<std::byte, kWsMaxControlPayload> payload{};
    const std::size_t n =
        ws_encode_close_payload(payload, static_cast<std::uint16_t>(code), reason);
    const bool ok = send_frame(WsOpcode::Close, std::span<const std::byte>(payload.data(), n));
    closing_ = true;
    return ok;
  }
  bool send_raw(WsOpcode op, bool fin, std::span<const std::byte> payload) noexcept override {
    return send_frame(op, payload, fin);
  }
  void close_abrupt() noexcept override { terminate(); }
  std::uint64_t id() const noexcept override { return id_; }
  std::string_view path() const noexcept override {
    return std::string_view(target_).substr(0, path_len_);
  }
  std::string_view query() const noexcept override {
    return path_len_ < target_.size() ? std::string_view(target_).substr(path_len_ + 1)
                                      : std::string_view{};
  }
  bool is_open() const noexcept override { return open_; }
  const WsStats& stats() const noexcept override { return stats_; }

  // --- IoHandler ---------------------------------------------------------------------------
  void on_readable() override {
    if (open_) read_frames();
  }
  void on_writable() override {
    if (open_) flush();
  }
  void on_error(int) override { fail(NetError::Syscall, "socket error"); }

  // --- WsMessageAssembler sink ---------------------------------------------------------------
  void on_message(WsOpcode op, std::span<std::byte> payload) noexcept {
    if (!open_) return;
    ++stats_.frames_rx;
    if (op == WsOpcode::Text) {
      handler_.on_text(
          *this, std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
    } else {
      handler_.on_binary(*this, payload);
    }
  }
  void on_control(WsOpcode op, std::span<std::byte> payload) noexcept {
    if (!open_) return;
    ++stats_.frames_rx;
    switch (op) {
      case WsOpcode::Ping:
        handler_.on_ping(*this, payload);
        send_frame(WsOpcode::Pong, payload);
        break;
      case WsOpcode::Pong:
        handler_.on_pong(*this, payload);
        break;
      case WsOpcode::Close: {
        const std::uint16_t code = ws_decode_close_code(payload);
        const std::string_view reason =
            payload.size() > 2 ? std::string_view(reinterpret_cast<const char*>(payload.data()) + 2,
                                                  payload.size() - 2)
                               : std::string_view{};
        if (!closing_) {
          std::array<std::byte, kWsMaxControlPayload> echo{};
          const std::size_t n = ws_encode_close_payload(
              echo, code == static_cast<std::uint16_t>(WsCloseCode::NoStatus) ? 1000 : code, {});
          send_frame(WsOpcode::Close, std::span<const std::byte>(echo.data(), n));
          if (!open_) break;  // the echo failed: on_error already fired
        }
        terminate();
        handler_.on_close(*this, code, reason);
        break;
      }
      default:
        break;
    }
  }

 private:
  void detach() noexcept {
    if (registered_) {
      reactor_.remove(stream_.fd());
      registered_ = false;
    }
  }

  void terminate() noexcept {
    if (!open_) return;
    open_ = false;
    detach();
    stream_.close();
    if (on_closed_) on_closed_(*this);
  }

  void fail(NetError err, const char* detail) noexcept {
    if (!open_) return;
    terminate();
    handler_.on_error(*this, err, detail);
  }

  bool send_frame(WsOpcode op, std::span<const std::byte> payload, bool fin = true) noexcept {
    if (!open_) return false;
    if (!ws_encode_frame(tx_, op, fin, payload, /*mask=*/nullptr)) {
      ++stats_.drops;
      return false;
    }
    ++stats_.frames_tx;
    flush();
    return true;
  }

  void flush() noexcept {
    if constexpr (requires(Stream& s) { s.flush(); }) {
      const IoResult fl = stream_.flush();
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

  void read_frames() {
    for (;;) {
      const std::span<std::byte> w = rx_.writable();
      if (w.empty()) {
        protocol_fail(WsCloseCode::MessageTooBig, "message larger than receive buffer");
        return;
      }
      const IoResult r = stream_.read(w);
      if (r.bytes > 0) {
        rx_.commit(r.bytes);
        stats_.bytes_rx += r.bytes;
        const detail::WsAssembleError err = assembler_.process(*this);
        if (err) {
          protocol_fail(err.code, err.detail);
          return;
        }
        if (!open_) return;
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

  void protocol_fail(WsCloseCode code, const char* detail) noexcept {
    if (open_ && !closing_) {
      std::array<std::byte, kWsMaxControlPayload> payload{};
      const std::size_t n = ws_encode_close_payload(payload, static_cast<std::uint16_t>(code), {});
      send_frame(WsOpcode::Close, std::span<const std::byte>(payload.data(), n));
    }
    fail(code == WsCloseCode::MessageTooBig ? NetError::Overflow : NetError::Protocol, detail);
  }

  Reactor& reactor_;
  Stream stream_;
  WsSessionHandler& handler_;
  std::uint64_t id_;
  std::string target_;
  std::size_t path_len_ = 0;
  RecvBuffer rx_;
  WireBuffer tx_;
  detail::WsMessageAssembler assembler_;
  WsStats stats_;
  std::array<char, detail::kWsAcceptLen> accept_{};
  ClosedFn on_closed_;
  bool registered_ = false;
  bool open_ = false;
  bool closing_ = false;
};

}  // namespace fastmm::net
