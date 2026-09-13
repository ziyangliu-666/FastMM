#include "fastmm/net/connection.hpp"

#include <stdexcept>

namespace fastmm::net {

std::string_view to_string(ConnState s) noexcept {
  switch (s) {
    case ConnState::Idle:
      return "Idle";
    case ConnState::Resolving:
      return "Resolving";
    case ConnState::Connecting:
      return "Connecting";
    case ConnState::TlsHandshake:
      return "TlsHandshake";
    case ConnState::WsHandshake:
      return "WsHandshake";
    case ConnState::Authenticating:
      return "Authenticating";
    case ConnState::Subscribing:
      return "Subscribing";
    case ConnState::Live:
      return "Live";
    case ConnState::Stale:
      return "Stale";
    case ConnState::Closing:
      return "Closing";
    case ConnState::Backoff:
      return "Backoff";
  }
  return "?";
}

void validate_connection_config(const ConnectionConfig& cfg, Url& url) {
  auto parsed = Url::parse(cfg.url);
  if (!parsed) throw std::invalid_argument("Connection: invalid url: " + cfg.url);
  if (parsed->scheme != "ws" && parsed->scheme != "wss") {
    throw std::invalid_argument("Connection: url scheme must be ws or wss: " + cfg.url);
  }
  if (cfg.stale_ms == 0 || cfg.dead_ms < cfg.stale_ms) {
    throw std::invalid_argument("Connection: require 0 < stale_ms <= dead_ms");
  }
  if (cfg.connect_timeout_ms == 0)
    throw std::invalid_argument("Connection: connect_timeout_ms must be > 0");
  url = *parsed;
}

}  // namespace fastmm::net
