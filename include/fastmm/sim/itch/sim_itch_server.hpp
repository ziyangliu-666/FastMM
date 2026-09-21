#pragma once
// SimItchServer: the Nasdaq-style simulator behind fastmm-sim-itch (ADR-0015, section 6).
//
//   one thread, one net::Reactor
//     ├─ MatchingEngine + MarketGenerator per symbol
//     ├─ feed      engine effects -> ITCH 5.0 -> moldudp::Transmitter -> sendmmsg to lines A/B
//     │            (per-line seeded drops, token-bucket pacing, heartbeats, End of Session)
//     ├─ UDP       MoldUDP64 re-request server answered from the Transmitter history
//     ├─ TCP       GLIMPSE 5.0 over SoupBinTCP: R, H, A per resting order, End of Snapshot
//     └─ TCP       OUCH 5.0 over SoupBinTCP: Enter / Replace / Cancel into the engines;
//                  Accepted / Replaced / Canceled / Executed / Rejected back, E and D on the feed
//
// Publishing (engine effect -> ITCH): an order that rests -> A with its leaves; an execution of
// a resting order -> E (the match number is shared with the OUCH Executed message); a cancel or
// the old leg of a re-entered replace -> D; a replace that keeps priority (same price, smaller or
// equal quantity) and an OUCH partial cancel -> X, keeping the order reference. OUCH Accepted
// carries the ITCH order reference number of the order.
//
// A GLIMPSE snapshot is built between two engine calls, so it reflects exactly the messages
// published up to End of Snapshot's sequence number minus one, sent or not.
//
// Wire-to-wire: every data datagram's first send is stamped with rdtscp right before the
// sendmmsg call that carries it; an Enter Order whose ClOrdID is a sequence token
// (ouch50::put_seq_token) is stamped with rdtscp right after the read that returned it, and the
// difference for the datagram holding that sequence number goes into a LogLinearHistogram.
//
// Threading: poll() on one thread. Nothing here is thread-safe.
#include "fastmm/core/latency.hpp"
#include "fastmm/sim/itch/sim_itch_config.hpp"
#include "fastmm/sim/matching_engine.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace fastmm::sim::itch {

struct SimItchStats {
  // feed
  std::uint64_t messages = 0;  // ITCH messages published
  std::uint64_t packets = 0;   // data datagrams built
  std::uint64_t datagrams_sent[2] = {0, 0};
  std::uint64_t datagrams_dropped[2] = {0, 0};  // by drop_rate
  std::uint64_t send_errors = 0;
  std::uint64_t heartbeats = 0;
  std::uint64_t history_evicted = 0;
  std::uint64_t publish_failures = 0;
  // re-request server
  std::uint64_t requests = 0;
  std::uint64_t requests_answered = 0;
  std::uint64_t requests_unanswered = 0;  // bad session, not in the history
  // GLIMPSE
  std::uint64_t glimpse_logins = 0;
  std::uint64_t snapshots = 0;
  std::uint64_t snapshot_orders = 0;
  std::uint64_t snapshot_overflows = 0;  // snapshot larger than glimpse_max_messages
  // OUCH
  std::uint64_t ouch_logins = 0;
  std::uint64_t orders = 0;
  std::uint64_t order_rejects = 0;
  std::uint64_t replaces = 0;
  std::uint64_t cancels = 0;
  std::uint64_t executions = 0;
  std::uint64_t ignored = 0;    // stale UserRefNum, unknown order, unsupported message
  std::uint64_t malformed = 0;  // inbound OUCH messages that do not parse
  // wire-to-wire
  std::uint64_t w2w_tokens = 0;  // Enter Orders with a sequence token
  std::uint64_t w2w_misses = 0;  // token not found in the stamp ring (too old, not sent yet)
  // generator
  std::uint64_t generator_actions = 0;
};

class SimItchServer {
 public:
  explicit SimItchServer(SimItchConfig cfg);  // throws std::invalid_argument
  ~SimItchServer();
  SimItchServer(const SimItchServer&) = delete;
  SimItchServer& operator=(const SimItchServer&) = delete;

  // Opens the multicast socket, the re-request socket and the listeners. False + last_error().
  // The first poll() publishes the opening spin (System Event O, Stock Directory, S, Q, Trading
  // Action T per symbol) and the seeded books.
  bool open();
  [[nodiscard]] const std::string& last_error() const noexcept;
  [[nodiscard]] const SimItchConfig& config() const noexcept;
  [[nodiscard]] std::uint16_t rerequest_port() const noexcept;  // 0 when off
  [[nodiscard]] std::uint16_t glimpse_port() const noexcept;
  [[nodiscard]] std::uint16_t ouch_port() const noexcept;

  // One iteration: network I/O (waiting at most max_wait_ms, less when the generator or the
  // pacing is due; never with busy_poll), generator steps, datagrams out.
  void poll(int max_wait_ms);
  // Sends End of Session on every line (after the pending data). The feed stops publishing.
  void end_session();

  // Stops (or restarts) the generators; OUCH orders still trade.
  void set_generator_enabled(bool on) noexcept;
  // True when every published message has been sent.
  [[nodiscard]] bool idle() const noexcept;
  [[nodiscard]] std::uint64_t published() const noexcept;  // last ITCH sequence number

  [[nodiscard]] std::size_t symbol_count() const noexcept;
  [[nodiscard]] const MatchingEngine& engine(std::size_t symbol) const noexcept;
  // ITCH order reference of a resting engine order (0 if unknown).
  [[nodiscard]] std::uint64_t order_ref(std::size_t symbol, std::uint64_t order_id) const noexcept;

  [[nodiscard]] SimItchStats stats() const noexcept;
  // Receive of an Enter Order minus send of the datagram carrying its token's sequence, in ns.
  [[nodiscard]] const LogLinearHistogram& wire_to_wire() const noexcept;
  // {"wire_to_wire_ns": {count, min, p50, p90, p99, p999, max, mean}, "misses", feed counters}.
  [[nodiscard]] std::string summary_json() const;

  struct Impl;  // src/sim/itch/sim_itch_impl.hpp

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace fastmm::sim::itch
