#pragma once
// Control-path plumbing every exchange connector repeats (6.4 / 6.5): the transport state the
// engine is told about, response-header parsing, URL origins, the venue kill switch, the
// published status and the free-form `extra` keys of a [venues.<name>] section. Nothing here
// runs per market-data message.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/net/connection.hpp"
#include "fastmm/net/http_client.hpp"
#include "fastmm/net/url.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/order_events.hpp"
#include "fastmm/venues/venue.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fastmm::venues {

// net::ConnState is the transport's view, one value per handshake step; ConnectionStateMsg
// carries the four the engine acts on. Every WebSocket connector collapses them this way.
[[nodiscard]] constexpr ConnState map_conn_state(net::ConnState s) noexcept {
  switch (s) {
    case net::ConnState::Live:
      return ConnState::Live;
    case net::ConnState::Stale:
      return ConnState::Stale;
    case net::ConnState::Resolving:
    case net::ConnState::Connecting:
    case net::ConnState::TlsHandshake:
    case net::ConnState::WsHandshake:
    case net::ConnState::Authenticating:
    case net::ConnState::Subscribing:
      return ConnState::Connecting;
    case net::ConnState::Idle:
    case net::ConnState::Closing:
    case net::ConnState::Backoff:
      return ConnState::Disconnected;
  }
  return ConnState::Disconnected;
}

// The same mapping in the status line's vocabulary (VenueStatus::md / user / order).
[[nodiscard]] constexpr ChannelState channel_state(net::ConnState s) noexcept {
  switch (map_conn_state(s)) {
    case ConnState::Live:
      return ChannelState::Live;
    case ConnState::Stale:
      return ChannelState::Stale;
    case ConnState::Connecting:
      return ChannelState::Connecting;
    default:
      return ChannelState::Down;
  }
}

// A user or order channel: quiet is normal there (no orders, no fills), and a dead one is caught
// by dead_ms and reconnected, so Stale is shown as Live rather than as a fault half the time.
[[nodiscard]] constexpr ChannelState private_channel_state(net::ConnState s) noexcept {
  const ChannelState c = channel_state(s);
  return c == ChannelState::Stale ? ChannelState::Live : c;
}

// Integer response header; -1 when it is absent or not a number (rate-limit headers use -1 for
// "the venue did not say").
[[nodiscard]] inline std::int64_t header_int(const net::HttpResponse& r,
                                             std::string_view name) noexcept {
  const std::string_view v = r.header(name);
  if (v.empty()) return -1;
  const auto parsed = parse_int64(v);
  return parsed ? *parsed : -1;
}

// "scheme://host[:port]" of a parsed URL; the scheme's default port is left out.
[[nodiscard]] inline std::string origin_of(const net::Url& u) {
  std::string s(u.scheme);
  s += "://";
  s += u.host;
  const bool default_port = (u.tls && u.port == 443) || (!u.tls && u.port == 80);
  if (!default_port) s += ":" + std::to_string(u.port);
  return s;
}

// origin_of() plus the URL's path without trailing slashes. Throws std::invalid_argument naming
// `venue` when the URL does not parse.
[[nodiscard]] inline std::string url_root(std::string_view venue, const std::string& url) {
  const auto u = net::Url::parse(url);
  if (!u) throw std::invalid_argument(std::string(venue) + ": bad URL " + url);
  std::string s = origin_of(*u);
  std::string_view path = u->path;
  while (!path.empty() && path.back() == '/') path.remove_suffix(1);
  s += path;
  return s;
}

// Decimal text of a venue order id, without touching the heap.
struct IdText {
  char buf[24];
  std::size_t n;
  explicit IdText(std::int64_t v) noexcept : n(format_int64(v, buf)) {}
  [[nodiscard]] std::string_view view() const noexcept { return {buf, n}; }
};

// The last status a connector published. The writer is its reactor thread and the reader a
// control thread: after 100 torn reads the caller gets a zeroed status rather than spin on.
[[nodiscard]] inline VenueStatus load_published_status(const Seqlocked<VenueStatus>& p) noexcept {
  VenueStatus s;
  for (int i = 0; i < 100; ++i) {
    if (p.try_load(s)) return s;
  }
  return s;
}

// First HardStop / Fatal error: the engine trips this venue's kill switch (quotes pulled, new
// orders refused by risk); the other venues keep trading. Sent once per session, so `sent` is
// the connector's own flag. Returns true when it sent, which is the connector's cue to log
// (a header cannot format a std::string_view before <fmt/format.h> is included).
[[nodiscard]] inline bool trip_venue_kill_once(bool& sent,
                                               EventSink* order_sink,
                                               VenueId venue,
                                               KillReason reason) noexcept {
  if (sent || order_sink == nullptr) return false;
  sent = true;
  emit_venue_kill(*order_sink, venue, reason);
  return true;
}

// The free-form `extra` keys of a [venues.<name>] section, read the same way by every
// make_*_config(). A missing key gives the default; a value that is not a number gives the
// default too (the schema check reports the typo).
class VenueExtras {
 public:
  explicit VenueExtras(const std::map<std::string, std::string>* extra) noexcept : extra_(extra) {}
  explicit VenueExtras(const std::map<std::string, std::string>& extra) noexcept : extra_(&extra) {}

  [[nodiscard]] std::string get(const char* key) const {
    if (extra_ == nullptr) return {};
    const auto it = extra_->find(key);
    return it == extra_->end() ? std::string{} : it->second;
  }
  [[nodiscard]] bool flag(const char* key, bool def) const {
    const std::string s = get(key);
    if (s.empty()) return def;
    return s == "true" || s == "1" || s == "yes";
  }
  [[nodiscard]] std::int64_t integer(const char* key, std::int64_t def) const {
    const std::string s = get(key);
    if (s.empty()) return def;
    const auto n = parse_int64(s);
    return n ? *n : def;
  }

 private:
  const std::map<std::string, std::string>* extra_ = nullptr;
};

}  // namespace fastmm::venues
