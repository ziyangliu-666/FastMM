#pragma once
// ByteStream: the transport abstraction the WebSocket/HTTP layers are templated on.
// PlainStream (TcpSocket), TlsStream<PlainStream> and MemoryTransport (tests) all model it.
//
//   read(buf)   -> bytes read; want_read when nothing is available; closed on EOF
//   write(buf)  -> bytes accepted; want_write when the kernel/peer buffer is full
//   handshake() -> ok when the stream is usable; want_read/want_write while it is still
//                  completing (TCP connect, TLS handshake); failed() on error
//   fd()        -> descriptor to register with the Reactor (-1 for in-memory streams)
//   close()     -> releases the descriptor; subsequent operations fail
#include "fastmm/net/io_result.hpp"
#include "fastmm/net/tcp_socket.hpp"

#include <concepts>
#include <cstddef>
#include <span>

namespace fastmm::net {

template <class S>
concept ByteStream = requires(S s, std::span<std::byte> rb, std::span<const std::byte> wb) {
  { s.read(rb) } noexcept -> std::same_as<IoResult>;
  { s.write(wb) } noexcept -> std::same_as<IoResult>;
  { s.handshake() } noexcept -> std::same_as<IoResult>;
  { s.fd() } noexcept -> std::same_as<int>;
  { s.close() } noexcept;
};

// TCP without encryption. handshake() completes a non-blocking connect: it must be called
// again after the socket becomes writable while it reports want_write.
class PlainStream {
 public:
  PlainStream() = default;
  explicit PlainStream(TcpSocket&& sock) noexcept : sock_(std::move(sock)) {}
  PlainStream(PlainStream&&) noexcept = default;
  PlainStream& operator=(PlainStream&&) noexcept = default;

  IoResult handshake() noexcept {
    if (!sock_.valid()) return IoResult::failure(NetError::InvalidState);
    if (!sock_.connecting()) return IoResult::done(0);
    // SO_ERROR is 0 both before completion and on success; getpeername() distinguishes.
    if (sock_.peer_addr().has_value()) {
      sock_.finish_connect();
      return IoResult::done(0);
    }
    if (const int err = sock_.pending_error(); err != 0) {
      sock_.finish_connect();
      return IoResult::syscall(err);
    }
    return IoResult::wants_write();
  }
  IoResult read(std::span<std::byte> buf) noexcept { return sock_.read(buf); }
  bool input_drained() const noexcept { return sock_.input_drained(); }
  IoResult write(std::span<const std::byte> buf) noexcept { return sock_.write(buf); }
  int fd() const noexcept { return sock_.fd(); }
  void close() noexcept { sock_.close(); }
  bool is_open() const noexcept { return sock_.valid(); }

  TcpSocket& socket() noexcept { return sock_; }
  const TcpSocket& socket() const noexcept { return sock_; }

 private:
  TcpSocket sock_;
};

static_assert(ByteStream<PlainStream>);

}  // namespace fastmm::net
