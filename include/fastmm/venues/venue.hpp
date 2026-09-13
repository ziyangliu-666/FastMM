#pragma once
// Venue: the control-path interface every exchange connector implements (6.2). Nothing here
// is on the hot path; the hot path lives in the connector's feed/gateway objects that the
// concrete Venue owns and drives from its reactor thread.
//
// Threading contract (see apps/fastmm-live/live_backend.hpp):
//   * load_reference_data() and attach() run once on the main thread before any reactor
//     thread starts (blocking REST allowed).
//   * connect/disconnect/subscribe/on_timer/on_wake/request_open_orders run on the venue's
//     reactor thread (the backend posts them there).
//   * cancel_all() must be callable from ANY thread, including when the reactor thread is
//     wedged: implementations use an independent blocking REST connection (6.7 kill switch).
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/result.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/symbology.hpp"
#include "fastmm/venues/wire_latency.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace fastmm::venues {

struct VenueCaps {
  bool supports_replace = false;   // Binance cancelReplace / Bybit amend
  bool supports_post_only = true;  // LIMIT_MAKER / PostOnly
  bool ws_order_entry = true;      // orders over WebSocket (REST fallback otherwise)
  bool user_stream = true;         // private order/fill stream available (false in dry-run)
};

enum class ChannelState : std::uint8_t { Down = 0, Connecting = 1, Live = 2, Stale = 3 };
[[nodiscard]] constexpr std::string_view to_string(ChannelState s) noexcept {
  switch (s) {
    case ChannelState::Down:
      return "Down";
    case ChannelState::Connecting:
      return "Connecting";
    case ChannelState::Live:
      return "Live";
    case ChannelState::Stale:
      return "Stale";
  }
  return "?";
}

// Snapshot for the stats line; every field is a plain counter so it can be copied out from
// the control thread (the venue publishes it through a Seqlocked).
struct VenueStatus {
  ChannelState md = ChannelState::Down;
  ChannelState user = ChannelState::Down;
  ChannelState order = ChannelState::Down;
  std::uint32_t books_synced = 0;
  std::uint32_t books_total = 0;
  std::uint64_t md_messages = 0;
  std::uint64_t md_malformed = 0;
  std::uint64_t md_dropped = 0;  // ring overflow
  std::uint64_t resyncs = 0;
  std::uint64_t orders_sent = 0;
  std::uint64_t cancels_sent = 0;
  std::uint64_t replaces_sent = 0;
  std::uint64_t order_events = 0;
  std::uint64_t order_send_failures = 0;
  std::uint64_t rest_requests = 0;
  std::uint64_t rest_errors = 0;
  std::uint64_t rate_limit_cooldowns = 0;
  std::int64_t clock_offset_ms = 0;  // venue - local
  std::uint64_t reconnects = 0;
  std::int64_t last_md_rx_ns = 0;  // reactor clock
  // Network-thread order latency (wire_latency.hpp), cumulative for the session. The ns
  // percentiles need a calibration source (Venue::set_tsc_calibration_source); counts do not.
  WireLatencyStats wire_tick_to_trade;  // inbound receive (t0_cycles) -> send call returned
  WireLatencyStats order_encode;        // JSON encoding + signing
  WireLatencyStats order_send;          // WebSocket write / REST request call
};

class Venue {
 public:
  virtual ~Venue() = default;

  [[nodiscard]] virtual VenueId id() const noexcept = 0;
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual VenueCaps caps() const noexcept = 0;

  // Fetches exchangeInfo / instruments-info for every instrument of this venue and
  // overwrites tick/lot/min_qty/max_qty/min_notional with the venue's values (config values
  // are a fallback when the venue is unreachable and `allow_offline` is set). Fatal on
  // mismatch of a required symbol.
  virtual Result<void, std::string> load_reference_data(InstrumentTable& instruments) = 0;

  // Wires the sinks (md: lossy, orders: never dropped) and the outbound ring the engine
  // writes Out*Msg into. Must be called before connect().
  virtual void attach(const SymbolTable& symbols,
                      const InstrumentTable& instruments,
                      EventSink& md_sink,
                      EventSink& order_sink,
                      MsgRing* outbound) = 0;

  // Opens every channel (market data, user stream, order entry) on `reactor`.
  virtual void connect(net::Reactor& reactor) = 0;
  virtual void disconnect() = 0;
  virtual void subscribe(std::span<const InstrumentId> instruments) = 0;

  // Periodic housekeeping (listenKey keepalive, app-level pings, clock offset refresh,
  // snapshot retries). The backend calls it about once per second; venues may also arm
  // their own reactor timers.
  virtual void on_timer(std::int64_t now_ns) = 0;
  // The engine pushed Out*Msg into the outbound ring: drain, encode, send.
  virtual void on_wake() = 0;

  // Venue view of open orders -> ReconcileMsg Begin/OpenOrder*/End into the order sink.
  virtual void request_open_orders() = 0;
  // Kill switch: cancel every open order on every subscribed symbol via an independent
  // REST connection. Blocking; safe from any thread. Returns false if the venue refused.
  virtual bool cancel_all() = 0;

  [[nodiscard]] virtual VenueStatus status() const noexcept = 0;

  // Published TSC calibrations used to convert the network thread's cycle histograms to ns
  // when the status is published. Set before connect(); the source must outlive the venue's
  // reactor thread.
  void set_tsc_calibration_source(const Seqlocked<TscCalibration>* src) noexcept {
    tsc_source_ = src;
  }

 protected:
  // Latest published calibration, or a calibration with use_tsc == false without a source.
  [[nodiscard]] TscCalibration tsc_calibration() const noexcept {
    return tsc_source_ != nullptr ? tsc_source_->load() : TscCalibration{};
  }

 private:
  const Seqlocked<TscCalibration>* tsc_source_ = nullptr;
};

}  // namespace fastmm::venues
