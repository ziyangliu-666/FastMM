#pragma once
// Endpoint selection: which of a venue host's addresses a connection goes to.
//
// A venue name often stands for many addresses, and from one host they are not equally near: a
// TCP connect to some takes 90 us longer than to the rest. The system resolver picks one of them
// at random. With selection on ([engine] / [venues.<name>] endpoint_select), a connection to a
// host name goes through the host's EndpointHost instead:
//
//   lookup   every lookup's addresses are added to the ones the host already had (a venue's DNS
//            answers rotate through a larger set); one that no lookup has returned for
//            forget_after_ns() is dropped
//   probe    `samples` TCP connects to each address, closed with a reset at once; its median
//            connect time ranks it. A connection's lookup probes (at start and at every
//            reconnect), and a background thread probes every host in use each
//            probe_interval_s, off the network threads
//   choose   a new connection takes the fastest address; addresses within spread_us of it count
//            as equally fast, and among those the one with the fewest connections on it, so
//            redundant copies of a stream spread over the fast addresses
//   fail     an address a connection to it failed on (connect, TLS, upgrade) is skipped for
//            kFailedHoldNs; the next one takes over
//   move     a live connection whose address has become switch_us slower than the fastest, or that
//            no lookup returns any longer, may move (Connection: when it can roll over, by
//            make-before-break)
//
// TLS keeps the host name for SNI and certificate checks, and the HTTP Host header keeps it too:
// only the address the socket connects to changes.
//
// Everything here runs at connect time or on helper threads and takes a mutex; the network thread
// only reads generation() (an atomic) between connects.
#include "fastmm/net/dns.hpp"
#include "fastmm/net/source_address.hpp"
#include "fastmm/net/tcp_socket.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fastmm::net {

struct EndpointSelectConfig {
  bool enabled = false;                   // the default of every venue that does not say
  std::uint32_t samples = 8;              // TCP connects per address and probe
  std::uint32_t probe_interval_s = 300;   // background probes; 0: only when a connection connects
  std::uint32_t spread_us = 20;           // as fast as the fastest, for spreading connections
  std::uint32_t switch_us = 50;           // a live connection this much slower moves; 0: never
  std::uint32_t probe_timeout_ms = 1000;  // one connect; past it the sample failed
  std::uint32_t probe_gap_us = 1000;      // between the background thread's connects
  std::uint32_t move_check_ms = 1000;     // how often a connection that can move looks
  // An address no lookup has returned for this long is dropped; 0: 3 probe intervals, at least
  // 10 minutes (a venue's answers rotate through more addresses than one answer holds).
  std::uint32_t forget_ms = 0;
};

// What a lookup and a probe do, replaceable for tests. resolve: the host's addresses (dns.hpp's
// resolve_sync by default); connect_ns: one connect's duration, ns, nullopt when it failed or
// timed out.
struct EndpointHooks {
  std::function<ResolveResult(std::string_view host, std::uint16_t port)> resolve;
  std::function<std::optional<std::int64_t>(
      const SockAddr& peer, const SourceAddress& source, std::uint32_t timeout_ms)>
      connect_ns;
};

// One TCP connect to `peer` (from `source`), timed: the time from connect() to the handshake's
// completion, ns. The socket is closed with a reset (no TIME_WAIT). Nullopt on failure or timeout.
[[nodiscard]] std::optional<std::int64_t> probe_connect_ns(const SockAddr& peer,
                                                           const SourceAddress& source,
                                                           std::uint32_t timeout_ms);

// One address of a host as last probed.
struct EndpointAddress {
  SockAddr addr;
  std::int64_t rtt_ns = -1;       // median connect time of the last probe; -1: none succeeded
  std::int64_t min_ns = -1;       // fastest connect of the last probe
  std::uint32_t samples = 0;      // connects of the last probe that succeeded
  std::uint32_t users = 0;        // connections on it now
  std::int64_t seen_ns = 0;       // the last lookup that returned it (monotonic clock)
  std::int64_t failed_until = 0;  // skipped until then: a connection to it failed
  bool probed = false;            // a probe has run since it was found
};

// The address a new connection takes.
struct EndpointChoice {
  SockAddr addr;
  std::int64_t rtt_ns = -1;     // its median connect time; -1: not measured
  std::int64_t best_ns = -1;    // the fastest address's
  std::uint32_t addresses = 0;  // addresses the host has
  std::uint32_t users = 0;      // connections on it, this one included when leased
};

class EndpointHost {
 public:
  // A connection to it failed: other addresses are taken first for this long.
  static constexpr std::int64_t kFailedHoldNs = 60'000'000'000;

  EndpointHost(std::string host,
               std::uint16_t port,
               SourceAddress source,
               EndpointSelectConfig cfg,
               EndpointHooks hooks);
  EndpointHost(const EndpointHost&) = delete;
  EndpointHost& operator=(const EndpointHost&) = delete;

  // Blocking (a resolver helper or the background thread): looks the host up, merges the answer
  // and probes every address. A foreground call that waited while another refresh ran uses that
  // one's result. Returns the addresses fastest first, unreachable ones last; gai_error only when
  // the lookup failed and no address is known.
  ResolveResult refresh(bool background = false);

  // The address for a new connection; nullopt when none is known. `lease`: it counts as one more
  // connection on the address until release().
  [[nodiscard]] std::optional<EndpointChoice> acquire(bool lease);
  void release(const SockAddr& addr) noexcept;
  // A connection to `addr` failed before it was up.
  void failed(const SockAddr& addr) noexcept;
  // A live connection on `addr` would do better elsewhere: the address has gone from the
  // lookups, or is switch_us slower than the fastest. False when there is nowhere better to go.
  [[nodiscard]] bool should_move(const SockAddr& addr) const noexcept;

  // Bumped by every refresh.
  [[nodiscard]] std::uint64_t generation() const noexcept {
    return generation_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool probed() const noexcept;
  [[nodiscard]] std::vector<EndpointAddress> addresses() const;  // fastest first
  [[nodiscard]] std::vector<SockAddr> ranked() const;            // the same, addresses only
  [[nodiscard]] const std::string& host() const noexcept { return host_; }
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] const SourceAddress& source() const noexcept { return source_; }
  [[nodiscard]] const EndpointSelectConfig& config() const noexcept { return cfg_; }
  // An address no lookup has returned for this long is dropped.
  [[nodiscard]] std::int64_t forget_after_ns() const noexcept;

 private:
  void sort_locked();
  std::vector<SockAddr> ranked_locked() const;
  const EndpointAddress* best_locked(std::int64_t now) const noexcept;

  std::string host_;
  std::uint16_t port_ = 0;
  SourceAddress source_;
  EndpointSelectConfig cfg_;
  EndpointHooks hooks_;
  std::mutex refresh_mutex_;  // one lookup and probe at a time
  mutable std::mutex mutex_;  // addrs_ and last_refresh_ns_
  std::vector<EndpointAddress> addrs_;
  std::int64_t last_refresh_ns_ = 0;
  std::atomic<std::uint64_t> generation_{0};
};

// The process's hosts, one EndpointHost per host, port and source address, and the background
// thread that probes the ones in use.
class EndpointSelector {
 public:
  static EndpointSelector& instance();

  // Before the venues connect (fastmm-live and fastmm-gateway at start). Hosts made afterwards
  // use these settings; tests pass hooks.
  void configure(const EndpointSelectConfig& cfg, EndpointHooks hooks = {});
  [[nodiscard]] EndpointSelectConfig config() const;
  // Whether a connection whose venue says `venue` (nullopt: nothing) selects its endpoint.
  [[nodiscard]] bool selects(std::optional<bool> venue) const noexcept {
    return venue.value_or(enabled_.load(std::memory_order_relaxed));
  }
  // The host's entry, made on first use; a host name only (nullptr for a numeric address). Kept
  // and probed in the background while someone holds it.
  [[nodiscard]] std::shared_ptr<EndpointHost> host(std::string_view host,
                                                   std::uint16_t port,
                                                   const SourceAddress& source);
  // One background round now, on the calling thread (tests).
  void probe_all();

  ~EndpointSelector();
  EndpointSelector(const EndpointSelector&) = delete;
  EndpointSelector& operator=(const EndpointSelector&) = delete;

 private:
  EndpointSelector() = default;
  void run(const std::stop_token& st);
  std::vector<std::shared_ptr<EndpointHost>> live_hosts();

  mutable std::mutex mutex_;
  std::condition_variable_any cv_;
  EndpointSelectConfig cfg_;
  EndpointHooks hooks_;
  std::atomic<bool> enabled_{false};
  std::map<std::string, std::weak_ptr<EndpointHost>> hosts_;
  std::jthread thread_;
};

// One log line for a connection that took `c` from `h`: "<what> <host>:<port> -> <ip> ...".
void log_endpoint_choice(std::string_view what, const EndpointHost& h, const EndpointChoice& c);

}  // namespace fastmm::net
