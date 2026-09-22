#pragma once
// ConnectionSlot<Handler>: owns either a plain-TCP or a TLS net::Connection depending on the
// URL scheme (ws:// for the local simulator, wss:// for the testnets) and forwards the
// control operations. The stream type is a template parameter of net::Connection, so this
// is the one place a runtime scheme choice is made; the handler callbacks stay statically
// dispatched.
#include "fastmm/net/connection.hpp"

#include <memory>
#include <string_view>
#include <utility>

namespace fastmm::venues {

template <net::ConnectionHandler Handler>
class ConnectionSlot {
 public:
  using Plain = net::Connection<net::PlainStream, Handler>;
  using Tls = net::Connection<net::TlsStream<net::PlainStream>, Handler>;

  ConnectionSlot() = default;
  ConnectionSlot(const ConnectionSlot&) = delete;
  ConnectionSlot& operator=(const ConnectionSlot&) = delete;

  // Throws std::invalid_argument on a bad URL (from validate_connection_config).
  void open(net::Reactor& reactor, net::ConnectionConfig cfg, Handler& handler) {
    reset();
    const auto url = net::Url::parse(cfg.url);
    if (url && url->tls) {
      tls_ = std::make_unique<Tls>(reactor, std::move(cfg), handler);
    } else {
      plain_ = std::make_unique<Plain>(reactor, std::move(cfg), handler);
    }
  }
  // Never call from inside the connection's own callbacks (net contract): the venue posts
  // the reset to the reactor instead.
  void reset() noexcept {
    plain_.reset();
    tls_.reset();
  }
  [[nodiscard]] bool opened() const noexcept { return plain_ || tls_; }

  void connect() {
    if (plain_) plain_->connect();
    if (tls_) tls_->connect();
  }
  void close() {
    if (plain_) plain_->close();
    if (tls_) tls_->close();
  }
  bool send_text(std::string_view t) noexcept {
    if (plain_) return plain_->send_text(t);
    if (tls_) return tls_->send_text(t);
    return false;
  }
  void cork() noexcept {
    if (plain_) plain_->cork();
    if (tls_) tls_->cork();
  }
  bool uncork() noexcept {
    if (plain_) return plain_->uncork();
    if (tls_) return tls_->uncork();
    return true;
  }
  void auth_done() {
    if (plain_) plain_->auth_done();
    if (tls_) tls_->auth_done();
  }
  void subscribe_done() {
    if (plain_) plain_->subscribe_done();
    if (tls_) tls_->subscribe_done();
  }
  [[nodiscard]] net::ConnState state() const noexcept {
    if (plain_) return plain_->state();
    if (tls_) return tls_->state();
    return net::ConnState::Idle;
  }
  [[nodiscard]] bool is_live() const noexcept {
    if (plain_) return plain_->is_live();
    if (tls_) return tls_->is_live();
    return false;
  }
  [[nodiscard]] net::ConnectionStats stats() const noexcept {
    if (plain_) return plain_->stats();
    if (tls_) return tls_->stats();
    return {};
  }
  [[nodiscard]] std::int64_t last_rx_ns() const noexcept {
    if (plain_) return plain_->last_rx_ns();
    if (tls_) return tls_->last_rx_ns();
    return 0;
  }

 private:
  std::unique_ptr<Plain> plain_;
  std::unique_ptr<Tls> tls_;
};

}  // namespace fastmm::venues
