#pragma once
// Venue: the control-path interface every exchange connector implements (6.2). Nothing here
// is on the hot path; the hot path lives in the connector's feed/gateway objects that the
// concrete Venue owns and drives from its reactor thread.
//
// Threading contract (see include/fastmm/live/session.hpp):
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
#include <utility>
#include <vector>

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

// Multicast feed (nasdaq_itch, ADR-0015 section 5); zero for the WebSocket venues.
enum class FeedState : std::uint8_t {
  None = 0,      // not a multicast venue
  Down = 1,      // not joined yet
  Snapshot = 2,  // joined and buffering; waiting for GLIMPSE (or sequence 1)
  Live = 3,      // books built, messages applied as they arrive
  Lost = 4,      // cannot rebuild the books: the venue's kill switch is tripped (FeedLost)
};
[[nodiscard]] constexpr std::string_view to_string(FeedState s) noexcept {
  switch (s) {
    case FeedState::None:
      return "none";
    case FeedState::Down:
      return "down";
    case FeedState::Snapshot:
      return "snapshot";
    case FeedState::Live:
      return "live";
    case FeedState::Lost:
      return "lost";
  }
  return "?";
}

struct VenueFeedStatus {
  FeedState state = FeedState::None;
  std::uint8_t backend = 0;   // 0 kernel, 1 af_xdp, 2 dpdk
  std::uint8_t xdp_mode = 0;  // net::XdpMode the first interface settled on (af_xdp)
  std::uint64_t packets = 0;  // MoldUDP64 packets accepted (all lines, retransmissions included)
  std::uint64_t bytes = 0;    // datagram payload bytes received
  std::uint64_t line_packets[2] = {0, 0};      // lines A, B
  std::uint64_t line_duplicates[2] = {0, 0};   // copies that arrived after the first
  std::int64_t line_skew_mean_ns[2] = {0, 0};  // arrival of a duplicate after the first copy
  std::int64_t line_skew_max_ns[2] = {0, 0};
  std::uint64_t gaps = 0;                 // gaps declared
  std::uint64_t recovered = 0;            // messages delivered from retransmissions
  std::uint64_t unrecovered = 0;          // sequences given up
  std::uint64_t snapshot_recoveries = 0;  // GLIMPSE snapshots applied after the first
  std::uint64_t recovery_overflows = 0;   // recovery buffer full during a snapshot
  std::uint64_t reorder_high_water = 0;
  std::uint64_t requests = 0;     // MoldUDP64 request packets sent
  std::uint64_t malformed = 0;    // datagrams that are not MoldUDP64 packets, bad frames (af_xdp)
  std::uint64_t book_errors = 0;  // L3 inconsistencies (each starts a resync)
  WireLatencyStats kernel_to_t0;  // t0_wall_ns - sw_ts_ns (kernel backend only)
  // af_xdp: XDP_STATISTICS summed over the sockets, and packets passed to the kernel because
  // their RX queue had no socket.
  std::uint64_t xdp_rx_dropped = 0;
  std::uint64_t xdp_rx_invalid_descs = 0;
  std::uint64_t xdp_rx_ring_full = 0;
  std::uint64_t xdp_fill_ring_empty = 0;
  std::uint64_t xdp_fallback = 0;
};

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
  std::uint64_t shadows_swept =
      0;  // order shadows a reconciliation proved dead (lost terminal events)
  std::uint64_t rest_requests = 0;
  std::uint64_t rest_errors = 0;
  std::uint64_t rate_limit_cooldowns = 0;
  std::int64_t clock_offset_ms = 0;  // venue - local
  std::uint64_t reconnects = 0;
  std::uint64_t executions_fetched = 0;  // trade-history rows replayed into the order sink
  std::uint64_t execution_queries = 0;
  std::uint64_t execution_query_errors = 0;  // a reconciliation that could not be made exact
  std::int64_t last_md_rx_ns = 0;            // reactor clock
  // Network-thread order latency (wire_latency.hpp), cumulative for the session. The ns
  // percentiles need a calibration source (Venue::set_tsc_calibration_source); counts do not.
  WireLatencyStats wire_tick_to_trade;  // inbound receive (t0_cycles) -> send call returned
  WireLatencyStats order_encode;        // JSON encoding + signing
  WireLatencyStats order_send;          // WebSocket write / REST request call
  VenueFeedStatus feed;                 // multicast venues only
};

// An order counted as sent whose batch was then never written (the connection failed at
// uncork()): the connector rejects it, so it is taken back out of the sent counters.
inline void unsend(VenueStatus& s, OrderCommandKind k) noexcept {
  switch (k) {
    case OrderCommandKind::New:
      --s.orders_sent;
      break;
    case OrderCommandKind::Cancel:
      --s.cancels_sent;
      break;
    case OrderCommandKind::Replace:
      --s.replaces_sent;
      break;
  }
}

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
  // Run-to-completion ([engine] threading = "single"): the engine runs on the reactor thread and
  // hands its Out*Msg batch over here instead of the outbound ring. Encode and write them now, as
  // on_wake() does; a message that cannot go out is refused through the order sink. Called from
  // inside the venue's own callbacks (a market-data event reaches the engine as it is decoded).
  virtual void send_now(std::span<const EventHeader* const> batch) = 0;
  // Called by the network thread after every reactor iteration. Venues whose sockets are not
  // registered with the reactor poll them here (nasdaq_itch with [engine] spin_mode = "busy").
  virtual void poll() noexcept {}
  // Every book starts over from a fresh snapshot: a ConnectionState{Resyncing} or Stale on the md
  // sink, then a snapshot per instrument. fastmm-gateway calls it when its own copies of the books
  // were lost (an attaching strategy gets snapshots of those copies instead). Reactor thread. The
  // default does nothing: a venue that cannot resync on demand (nasdaq_itch) leaves the books to
  // its next natural resync.
  virtual void resync_books() {}

  // Venue view of open orders -> ReconcileMsg Begin/OpenOrder*/End into the order sink. A
  // connector that can fetch executions (VenueCapabilities::executions) replays everything the
  // account traded since the watermark first, as ordinary OrderFillMsg carrying the venue's
  // execution id and OrderFillMsg::kReplayed, and only then emits the snapshot with
  // ReconcileMsg::kExecutionsExact on its Begin. That order is what closes the hole: a fill that
  // finished an order is booked from the execution itself, so the snapshot - which no longer
  // mentions the order at all - has nothing left to explain.
  virtual void request_open_orders() = 0;
  // The account's executions from `since_venue_ms` (the venue's clock, inclusive) onwards, for
  // every subscribed instrument: the venue's trade-history query (myTrades / userTrades /
  // execution/list / get_user_trades_by_currency_and_time). Each one is emitted into the order sink
  // as an ordinary OrderFillMsg carrying the venue's execution id and OrderFillMsg::kReplayed, so
  // the OMS books only the ones it has not seen. Zero means "from wherever this connector last got
  // to", which is what a reconciliation uses; a caller that knows better - the time of the last
  // execution the engine booked, which the store and the journal both hold - passes it.
  //
  // Asynchronous and part of a reconciliation: the connector runs it before the open-order snapshot
  // and stamps ReconcileMsg::kExecutionsExact on the snapshot's Begin when every instrument
  // answered in full. Returns false when this connector cannot ask (VenueCapabilities::executions
  // is false) or has nowhere to send the query, which is the only case where a fill the private
  // stream missed stays an estimate.
  virtual bool request_executions(std::int64_t /*since_venue_ms*/ = 0) { return false; }
  // Where the first execution replay starts, for a session that carries a position over from an
  // earlier one (fastmm-live restores it from the store before connect()): executions from
  // `since_venue_ms` onwards are booked, except those whose venue trade id is in `known`, which the
  // earlier session booked already. A connector that cannot replay executions ignores it, and the
  // session does not restore a position for it (VenueCapabilities::executions).
  virtual void resume_executions(std::int64_t /*since_venue_ms*/,
                                 const std::vector<std::string>& /*known*/) {}
  // An exact start where the venue's trade ids increase per instrument (Binance): the first trade
  // id to ask for, per instrument - one past the last the earlier session booked. Called after
  // resume_executions(); an instrument listed here replays from its id and ignores the time and
  // the known ids, the others go on from `since_venue_ms`. Connectors without such ids ignore it.
  virtual void resume_trade_ids(
      const std::vector<std::pair<InstrumentId, std::int64_t>>& /*next_ids*/) {}
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
