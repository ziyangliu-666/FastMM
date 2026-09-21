// TcpLink (see tcp_link.hpp).
#include "fastmm/venues/nasdaq/tcp_link.hpp"

#include <sys/socket.h>

#include <cerrno>
#include <cstring>

namespace fastmm::venues::nasdaq {

TcpLink::TcpLink(TcpLinkHandler& handler, std::size_t rx_bytes, std::size_t tx_bytes)
    : h_(handler),
      rx_(new std::byte[rx_bytes]),
      rx_cap_(rx_bytes),
      tx_(new std::byte[tx_bytes]),
      tx_cap_(tx_bytes) {}

TcpLink::~TcpLink() {
  close();
}

bool TcpLink::open(net::Reactor& reactor, const net::SockAddr& addr) noexcept {
  close();
  reactor_ = &reactor;
  net::TcpSocket s = net::TcpSocket::open(addr.family());
  if (!s.valid()) return false;
  const net::ConnectStatus st = s.connect(addr);
  if (st == net::ConnectStatus::Error) return false;
  static_cast<void>(s.set_nodelay(true));
  {
    const std::lock_guard<std::mutex> lock(fd_mutex_);
    sock_ = std::move(s);
  }
  ++gen_;
  rx_len_ = 0;
  tx_len_ = 0;
  up_ = false;
  writing_ = true;  // connect completion is reported as writability
  if (!reactor.add(sock_.fd(), *this, net::IoEvent::ReadWrite)) {
    close();
    return false;
  }
  if (st == net::ConnectStatus::Connected) {
    up_ = true;
    want_write(false);
    h_.on_link_up();
  }
  return true;
}

void TcpLink::close() noexcept {
  if (!sock_.valid()) return;
  if (reactor_ != nullptr) reactor_->remove(sock_.fd());
  ++gen_;
  const std::lock_guard<std::mutex> lock(fd_mutex_);
  sock_.close();
  up_ = false;
  writing_ = false;
  rx_len_ = 0;
  tx_len_ = 0;
}

bool TcpLink::shutdown_from_any_thread() noexcept {
  const std::lock_guard<std::mutex> lock(fd_mutex_);
  if (!sock_.valid()) return false;
  return ::shutdown(sock_.fd(), SHUT_RDWR) == 0;
}

void TcpLink::fail(int err) noexcept {
  close();
  h_.on_link_down(err);
}

void TcpLink::want_write(bool on) noexcept {
  if (writing_ == on || !sock_.valid() || reactor_ == nullptr) return;
  writing_ = on;
  reactor_->modify(sock_.fd(), *this, on ? net::IoEvent::ReadWrite : net::IoEvent::Read);
}

bool TcpLink::send(std::span<const std::byte> bytes) noexcept {
  if (!up_) return false;
  if (tx_len_ == 0) {
    while (!bytes.empty()) {
      const net::IoResult r = sock_.write(bytes);
      if (r.failed() || r.closed) {
        fail(r.failed() ? EPIPE : 0);
        return false;
      }
      if (r.would_block() || r.bytes == 0) break;
      bytes = bytes.subspan(r.bytes);
    }
    if (bytes.empty()) return true;
  }
  if (tx_cap_ - tx_len_ < bytes.size()) {
    fail(ENOBUFS);
    return false;
  }
  std::memcpy(tx_.get() + tx_len_, bytes.data(), bytes.size());
  tx_len_ += bytes.size();
  want_write(true);
  return true;
}

bool TcpLink::flush() noexcept {
  std::size_t off = 0;
  while (off < tx_len_) {
    const net::IoResult r = sock_.write(std::span<const std::byte>(tx_.get() + off, tx_len_ - off));
    if (r.failed() || r.closed) {
      fail(r.failed() ? EPIPE : 0);
      return false;
    }
    if (r.would_block() || r.bytes == 0) break;
    off += r.bytes;
  }
  if (off > 0) {
    std::memmove(tx_.get(), tx_.get() + off, tx_len_ - off);
    tx_len_ -= off;
  }
  return true;
}

void TcpLink::on_writable() {
  if (!sock_.valid()) return;
  if (!up_) {
    if (const int err = sock_.finish_connect(); err != 0) {
      fail(err);
      return;
    }
    up_ = true;
    const std::uint64_t gen = gen_;
    h_.on_link_up();
    if (gen != gen_) return;  // closed or reopened by the handler
  }
  if (!flush()) return;
  if (tx_len_ == 0) want_write(false);
}

void TcpLink::on_readable() {
  if (!sock_.valid()) return;
  if (!up_) {
    // A refused connect reports readability too; finish_connect() tells.
    if (const int err = sock_.finish_connect(); err != 0) {
      fail(err);
      return;
    }
    up_ = true;
    const std::uint64_t gen = gen_;
    h_.on_link_up();
    if (gen != gen_) return;
  }
  for (;;) {
    if (rx_len_ == rx_cap_) {
      fail(ENOBUFS);  // a packet larger than the buffer
      return;
    }
    const net::IoResult r =
        sock_.read(std::span<std::byte>(rx_.get() + rx_len_, rx_cap_ - rx_len_));
    if (r.bytes > 0) {
      rx_len_ += r.bytes;
      const std::uint64_t gen = gen_;
      const std::size_t used = h_.on_link_data(std::span<const std::byte>(rx_.get(), rx_len_));
      if (gen != gen_) return;  // the handler closed or reopened the link
      if (used > 0) {
        std::memmove(rx_.get(), rx_.get() + used, rx_len_ - used);
        rx_len_ -= used;
      }
    }
    if (r.closed) {
      fail(0);
      return;
    }
    if (r.failed()) {
      fail(ECONNRESET);
      return;
    }
    if (r.would_block() || r.bytes == 0) return;
  }
}

void TcpLink::on_error(int err) {
  fail(err != 0 ? err : ECONNRESET);
}

}  // namespace fastmm::venues::nasdaq
