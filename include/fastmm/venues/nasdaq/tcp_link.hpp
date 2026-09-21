#pragma once
// TcpLink: one plain TCP connection on a net::Reactor for the SoupBinTCP sessions of the
// nasdaq_itch venue (GLIMPSE, OUCH). Non-blocking connect, a receive buffer the owner consumes
// whole SoupBinTCP packets from, and a send buffer for the bytes the kernel did not take. Both
// buffers are allocated by the constructor; nothing allocates afterwards.
//
// send() satisfies soupbin::ByteWriter. Callbacks run on the reactor thread:
//   on_link_up()        the connect completed
//   on_link_data(bytes) bytes received so far (including earlier unconsumed ones); returns the
//                       bytes consumed
//   on_link_down(err)   the connection failed or closed (err: errno, 0 for EOF); not called for
//                       close()
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/tcp_socket.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

namespace fastmm::venues::nasdaq {

class TcpLinkHandler {
 public:
  virtual ~TcpLinkHandler() = default;
  virtual void on_link_up() noexcept = 0;
  virtual std::size_t on_link_data(std::span<const std::byte> bytes) noexcept = 0;
  virtual void on_link_down(int err) noexcept = 0;
};

class TcpLink final : public net::IoHandler {
 public:
  TcpLink(TcpLinkHandler& handler, std::size_t rx_bytes, std::size_t tx_bytes);
  ~TcpLink() override;
  TcpLink(const TcpLink&) = delete;
  TcpLink& operator=(const TcpLink&) = delete;
  TcpLink(TcpLink&&) = delete;
  TcpLink& operator=(TcpLink&&) = delete;

  // Starts a connect to `addr`; false when the socket cannot be created or registered.
  bool open(net::Reactor& reactor, const net::SockAddr& addr) noexcept;
  // Unregisters and closes; no callback.
  void close() noexcept;
  // Thread-safe: shuts the connection down (SHUT_RDWR) without closing the descriptor, so the
  // reactor thread sees EOF and closes it. For cancel-all from another thread.
  bool shutdown_from_any_thread() noexcept;

  // Queues bytes; false when not connected or the send buffer is full (the link is then closed
  // and on_link_down reports ENOBUFS).
  bool send(std::span<const std::byte> bytes) noexcept;

  [[nodiscard]] bool connected() const noexcept { return up_; }
  [[nodiscard]] bool is_open() const noexcept { return sock_.valid(); }

  void on_readable() override;
  void on_writable() override;
  void on_error(int err) override;

 private:
  void fail(int err) noexcept;
  bool flush() noexcept;
  void want_write(bool on) noexcept;

  TcpLinkHandler& h_;
  net::Reactor* reactor_ = nullptr;
  net::TcpSocket sock_;
  std::mutex fd_mutex_;  // close() vs shutdown_from_any_thread()
  std::unique_ptr<std::byte[]> rx_;
  std::size_t rx_cap_;
  std::size_t rx_len_ = 0;
  std::unique_ptr<std::byte[]> tx_;
  std::size_t tx_cap_;
  std::size_t tx_len_ = 0;
  std::uint64_t gen_ = 0;  // bumped by open() and close(): a callback may reopen the link
  bool up_ = false;
  bool writing_ = false;  // registered for Write
};

}  // namespace fastmm::venues::nasdaq
