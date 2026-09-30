#pragma once
// SimTransport (8.2): the TransportLike that stands in for the venues during backtests.
//
//   engine --send(Out*)--> EventScheduler (now + order_out) --> venue side at fire_ts:
//       FillModel::Matching : MatchingEngine, account 1 (strategy) vs account 0 (flow)
//       FillModel::L2Queue  : QueuePositionModel against a mirror of the historical book and the
//                             latest BookTicker when it is newer than that book
//   venue --acks/fills--> order wire (arrival = max(prev, t + ack_in)) --> InlineFeed
//   venue --market data--> md wire   (arrival = max(prev, t + md_in))  --> InlineFeed
//
// Every venue an instrument names is simulated with its own latency model, its own pair of wires,
// cancel-replace and STP (SimTransportConfig::venues). A message goes through the venue of its
// instrument. The wires model each venue's two TCP streams: messages arrive in order, so arrivals
// are clamped monotone per wire, and a slow venue never holds back a fast one. Books, the matching
// engine and the queue model are shared: instruments never span venues. SimDriver decides when to
// move a wire message into the engine's feed (when the virtual clock reaches its recv_ts); on a tie
// order wires come before md wires, lower venue ids first.
//
// Market data reaches the strategy in one of two ways:
//   * coupled generator: a MarketGenerator drives the MatchingEngine and the MdAggregator
//     publishes Binance-style 100 ms depth batches of the shared book (strategy orders
//     included, fills are real matches);
//   * source data: on_source_event() applies each historical event to the venue-side
//     state (mirror book + queue model, or account-0 liquidity in the matching engine so
//     the book trades through resting strategy orders) and forwards it to the engine.
#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/transport.hpp"
#include "fastmm/sim/event_scheduler.hpp"
#include "fastmm/sim/fee_model.hpp"
#include "fastmm/sim/latency_model.hpp"
#include "fastmm/sim/matching_engine.hpp"
#include "fastmm/sim/md_aggregator.hpp"
#include "fastmm/sim/outbound_hash.hpp"
#include "fastmm/sim/queue_model.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace fastmm::sim {

enum class FillModel : std::uint8_t { Matching = 0, L2Queue = 1 };
[[nodiscard]] constexpr std::string_view to_string(FillModel m) noexcept {
  return m == FillModel::Matching ? "matching" : "l2_queue";
}

// One venue's own settings. A venue without an entry in SimTransportConfig::venues takes the
// SimTransportConfig fields of the same names.
struct SimVenueConfig {
  VenueId venue{};
  LatencyParams order_out{microseconds(200), microseconds(50)};
  LatencyParams ack_in{microseconds(200), microseconds(50)};
  LatencyParams md_in{};
  bool md_recorded_arrival = false;
  bool supports_replace = false;
  StpMode stp = StpMode::None;
};

struct SimTransportConfig {
  LatencyParams order_out{microseconds(200), microseconds(50)};
  LatencyParams ack_in{microseconds(200), microseconds(50)};
  LatencyParams md_in{};
  // Historical market data arrives at its recorded recv_ts (plus md_in) instead of its venue time
  // (plus md_in), so the feed lag of the recording, and the staleness it brought, are replayed.
  // Messages keep their order: one that was recorded earlier than its predecessor waits for it.
  bool md_recorded_arrival = false;
  std::uint64_t seed = 1;
  FillModel fill_model = FillModel::Matching;
  std::int64_t queue_conservatism_bps = 10'000;
  FeeModel fees{};
  bool supports_replace = false;
  StpMode stp = StpMode::None;  // applied to the strategy account
  MdAggregatorConfig md{};      // coupled-generator mode
  // SHA-256 over every outbound message (outbound_hash()), used by the determinism and replay
  // checks. It costs about 70 ns per order, more than the engine work that produced it, so a
  // benchmark that times send() turns it off; backtests and tests leave it on.
  bool hash_outbound = true;
  std::size_t md_wire_bytes = 4U << 20;
  std::size_t order_wire_bytes = 1U << 20;
  VenueId venue{0};  // venue of the instruments that name none (and of an empty table)
  std::vector<SimVenueConfig> venues;

  // The settings `v` runs with: its entry in `venues`, or the fields above.
  [[nodiscard]] SimVenueConfig venue_config(VenueId v) const noexcept {
    for (const SimVenueConfig& c : venues) {
      if (c.venue == v) return c;
    }
    return SimVenueConfig{v, order_out, ack_in, md_in, md_recorded_arrival, supports_replace, stp};
  }
  // Bit v set: venue v uses cancel-replace (the journal header's replace_venues).
  [[nodiscard]] std::uint64_t replace_mask() const noexcept {
    std::uint64_t m = supports_replace ? ~std::uint64_t{0} : 0;
    for (const SimVenueConfig& c : venues) {
      if (!c.venue.valid() || c.venue.value >= 64) continue;
      const std::uint64_t bit = std::uint64_t{1} << c.venue.value;
      m = c.supports_replace ? (m | bit) : (m & ~bit);
    }
    return m;
  }
};

struct SimTransportStats {
  std::uint64_t orders_sent = 0;
  std::uint64_t cancels_sent = 0;
  std::uint64_t replaces_sent = 0;
  std::uint64_t dropped = 0;         // lost by the latency model's p_drop
  std::uint64_t scheduler_full = 0;  // send() returned false
  std::uint64_t wire_full = 0;       // venue -> engine message dropped (should be 0)
  std::uint64_t acks = 0;
  std::uint64_t rejects = 0;
  // Rejects broken down by cause; these five always sum to `rejects`.
  std::uint64_t rejects_post_only = 0;   // PostOnlyWouldCross: crossed the live book on arrival
  std::uint64_t rejects_level_full = 0;  // VenueReject: simulated price-level table full
  std::uint64_t rejects_invalid = 0;     // InvalidTick / InvalidLot / InstrumentDisabled
  std::uint64_t rejects_duplicate = 0;   // DuplicateId
  std::uint64_t rejects_other = 0;
  std::uint64_t fills = 0;
  std::uint64_t cancel_acks = 0;
  std::uint64_t cancel_rejects = 0;
  std::uint64_t expired = 0;
  std::uint64_t md_forwarded = 0;
  std::uint64_t md_delivered = 0;
  std::uint64_t order_events_delivered = 0;
  Notional fees_charged{};
};

// The venue's own view of the book at the moment of a fill, for fill-quality reporting.
struct FillContext {
  Price mid;       // venue mid; zero when the book has no two sides
  Price best_bid;  // venue best bid / ask, to tell a fill at the touch from one behind it
  Price best_ask;
  Qty queue_ahead;           // displayed quantity still ahead of us just before the fill
  bool queue_known = false;  // only FillModel::L2Queue tracks a queue position
};

// Result-side hooks (backtest collectors). Called on the venue side, in virtual time.
class SimObserver {
 public:
  virtual ~SimObserver() = default;
  // `venue_ts` is the scheduled arrival at the venue; invalid (0) when the latency model
  // dropped the message or the scheduler was full.
  virtual void on_order_sent(const EventHeader&, Timestamp /*send_ts*/, Timestamp /*venue_ts*/) {}
  virtual void on_fill(const OrderFillMsg&, Timestamp /*venue_ts*/, const FillContext&) {}
  virtual void on_order_event(const EventHeader&, Timestamp /*venue_ts*/) {}
};

class SimTransport final : public MatchingSink {
 public:
  static constexpr std::size_t kSchedulerCapacity = 1U << 14;
  static constexpr std::uint32_t kOutSlotBytes = 192;  // largest Out*Msg

  SimTransport(const SimClock& clock,
               const InstrumentTable& instruments,
               const SimTransportConfig& cfg);
  SimTransport(const SimTransport&) = delete;  // link_of_inst_ points into links_
  SimTransport& operator=(const SimTransport&) = delete;

  // ---- TransportLike ------------------------------------------------------------------------
  [[nodiscard]] bool send(const EventHeader& m) noexcept;
  [[nodiscard]] std::size_t send(std::span<const EventHeader* const> batch) noexcept;
  [[nodiscard]] bool supports_replace(VenueId v) const noexcept {
    return v.value < kMaxVenues ? replace_[v.value] : cfg_.supports_replace;
  }

  // ---- venue side (driven by SimDriver in virtual-time order) -------------------------------
  // Enables the coupled-generator market-data path (MdAggregator publishes the shared book).
  void enable_aggregator(Timestamp start) noexcept;
  [[nodiscard]] bool aggregator_enabled() const noexcept { return agg_ != nullptr; }
  [[nodiscard]] Timestamp next_order_arrival() const noexcept { return sched_.peek_ts(); }
  void process_order_arrival() noexcept;  // pops the earliest scheduled outbound message
  [[nodiscard]] Timestamp next_flush_ts() const noexcept {
    return agg_ == nullptr ? Timestamp::max() : agg_->next_flush_ts();
  }
  void flush_md(Timestamp now) noexcept;  // aggregator flush (coupled mode)
  // Historical event at venue time (hdr.exch_ts, falling back to recv_ts): updates the
  // venue-side fill model and forwards the event to the engine after md_in latency.
  void on_source_event(const EventHeader& md) noexcept;

  // ---- engine side --------------------------------------------------------------------------
  [[nodiscard]] Timestamp next_inbound_ts() noexcept;
  // Moves the earliest wire message into `feed`; returns its type (Padding if none).
  EventType deliver_next_inbound(InlineFeed& feed) noexcept;
  [[nodiscard]] bool inbound_pending() noexcept { return next_inbound_ts() != Timestamp::max(); }

  // ---- queries ------------------------------------------------------------------------------
  [[nodiscard]] MatchingEngine& matching_engine() noexcept { return me_; }
  [[nodiscard]] const MatchingEngine& matching_engine() const noexcept { return me_; }
  [[nodiscard]] const L2Book<256>& mirror(InstrumentId id) const noexcept {
    return mirror_[id.value];
  }
  [[nodiscard]] Price venue_mid(InstrumentId id) const noexcept;
  // Best price on one side of the venue book, zero when that side is empty. Same book as
  // venue_mid(): the mirror of the historical levels under L2Queue, the matching engine's own
  // book (strategy orders included) under Matching.
  [[nodiscard]] Price venue_best(InstrumentId id, Side side) const noexcept;
  [[nodiscard]] MatchingEngine::SideExposure strategy_exposure(InstrumentId id,
                                                               Side side) const noexcept;
  [[nodiscard]] const SimTransportStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const SimTransportConfig& config() const noexcept { return cfg_; }
  // The simulated venues, lowest id first; each instrument's venue is one of them.
  [[nodiscard]] std::size_t venue_count() const noexcept { return n_links_; }
  [[nodiscard]] VenueId venue_at(std::size_t k) const noexcept { return at(k).id; }
  // Latency model of venue `v` (the first venue when `v` is not simulated).
  [[nodiscard]] LatencyModel& latency(VenueId v) noexcept;
  [[nodiscard]] LatencyModel& latency() noexcept { return at(0).lat; }
  [[nodiscard]] const OutboundHasher& outbound_hash() const noexcept { return hasher_; }
  [[nodiscard]] const QueuePositionModel& queue() const noexcept { return queue_; }
  void set_observer(SimObserver* o) noexcept { observer_ = o; }

  // ---- MatchingSink (venue events for account 1 become engine messages) -------------------
  void on_ack(const SimOrder& o, Timestamp ts) override;
  void on_reject(const NewOrder& o, RejectReason r, Timestamp ts) override;
  void on_cancel(const SimOrder& o, CancelReason r, Timestamp ts) override;
  void on_cancel_reject(AccountId a, ClientOrderId id, InstrumentId inst, Timestamp ts) override;
  void on_fill(
      const SimOrder& maker, const SimOrder& taker, Price px, Qty qty, Timestamp ts) override;
  void on_book_change(InstrumentId id, Side s, Price px, Qty qty, std::uint64_t uid) override;
  void on_trade(
      InstrumentId id, Price px, Qty qty, Side aggr, std::uint64_t tid, Timestamp ts) override;

 private:
  struct OutSlot {
    std::uint32_t len;
    alignas(8) std::byte bytes[kOutSlotBytes];
  };
  // One simulated venue: its latency model and its two wires to the engine.
  struct Link {
    Link(VenueId v, const SimVenueConfig& c, std::uint64_t seed, const SimTransportConfig& t)
        : id(v),
          lat(c.order_out, c.ack_in, c.md_in, seed),
          md_wire(t.md_wire_bytes),
          order_wire(t.order_wire_bytes),
          md_recorded_arrival(c.md_recorded_arrival) {}
    VenueId id;
    LatencyModel lat;
    MsgRing md_wire;
    MsgRing order_wire;
    Timestamp last_md_arrival{};
    Timestamp last_order_arrival{};
    bool md_recorded_arrival;
  };
  using Scheduler = EventScheduler<OutSlot, kSchedulerCapacity>;

  // venue-side order handling
  void venue_new(const OutNewOrderMsg& m, Timestamp now) noexcept;
  void venue_cancel(const OutCancelMsg& m, Timestamp now) noexcept;
  void venue_replace(const OutReplaceMsg& m, Timestamp now) noexcept;
  // L2Queue fill model
  void queue_new(const NewOrder& n, Timestamp now) noexcept;
  void queue_cancel(ClientOrderId id, InstrumentId route, Timestamp now) noexcept;
  void queue_replace(const OutReplaceMsg& m, Timestamp now) noexcept;
  void queue_on_delta(const BookDeltaMsg& d, Timestamp now) noexcept;
  void queue_on_trade(const TradeMsg& t, Timestamp now) noexcept;
  void queue_on_ticker(const BookTickerMsg& m) noexcept;
  // Matching model fed with historical levels (account-0 liquidity mirror)
  void mirror_on_delta(const BookDeltaMsg& d, Timestamp now) noexcept;
  // Moves account-0 liquidity at (id, side, px) towards `target`: the decrease pass only
  // removes, the increase pass only adds.
  void sync_level(
      InstrumentId id, Side side, Price px, Qty target, Timestamp now, bool increases) noexcept;

  // venue -> engine messages
  void emit_ack(ClientOrderId id, std::uint64_t order_id, InstrumentId inst, Timestamp ts) noexcept;
  void emit_reject(ClientOrderId id, InstrumentId inst, RejectReason r, Timestamp ts) noexcept;
  void emit_cancel_ack(
      ClientOrderId id, std::uint64_t order_id, InstrumentId inst, Qty cum, Timestamp ts) noexcept;
  // `route`: the instrument whose venue answers (the cancel's), when `inst` is not known.
  void emit_cancel_reject(ClientOrderId id,
                          InstrumentId inst,
                          Timestamp ts,
                          InstrumentId route = InstrumentId{}) noexcept;
  void emit_expired(
      ClientOrderId id, std::uint64_t order_id, InstrumentId inst, Qty cum, Timestamp ts) noexcept;
  void emit_fill(ClientOrderId id,
                 std::uint64_t order_id,
                 InstrumentId inst,
                 Side side,
                 Price px,
                 Qty qty,
                 Qty cum,
                 Qty leaves,
                 Liquidity liq,
                 std::uint64_t exec_id,
                 Timestamp ts,
                 Qty queue_ahead = Qty{},
                 bool queue_known = false) noexcept;
  void push_order_wire(Link& l, EventHeader& h, Timestamp venue_ts) noexcept;
  void push_md_wire(EventHeader& h, Timestamp venue_ts) noexcept;
  // Venue of an instrument; an unknown one (a cancel of an order the venue never saw) goes
  // through the first venue.
  [[nodiscard]] Link& link(InstrumentId id) noexcept {
    return id.value < kMaxInstruments ? *link_of_inst_[id.value] : at(0);
  }
  // links_[k] for k < n_links_, which the constructor engaged; [0] always is.
  [[nodiscard]] Link& at(std::size_t k) noexcept {
    return *links_[k];  // NOLINT(bugprone-unchecked-optional-access)
  }
  [[nodiscard]] const Link& at(std::size_t k) const noexcept {
    return *links_[k];  // NOLINT(bugprone-unchecked-optional-access)
  }
  static void emit_md_thunk(void* ctx, EventHeader& m, Timestamp venue_ts) noexcept;
  [[nodiscard]] static Timestamp head_ts(MsgRing& ring) noexcept;
  bool move_head(MsgRing& ring, InlineFeed& feed, EventType& type) noexcept;

  const SimClock& clock_;
  const InstrumentTable& instruments_;
  SimTransportConfig cfg_;
  MatchingEngine me_;
  // The simulated venues in id order, [0, n_links_) engaged; inline, so the first venue's latency
  // model and wires sit where a single venue's always did.
  std::optional<Link> links_[kMaxVenues];
  std::size_t n_links_ = 0;
  // Instrument id -> its venue's link (the first venue's for an id no instrument has): one load
  // on every message, where an index into links_ took a multiply and two adds.
  Link* link_of_inst_[kMaxInstruments] = {};
  bool replace_[kMaxVenues] = {};
  Scheduler sched_;
  std::unique_ptr<L2Book<256>[]> mirror_;
  QueuePositionModel queue_;
  std::unique_ptr<QueueTouch[]> touch_;  // L2Queue: the latest BookTicker per instrument
  std::unique_ptr<TradeTape[]> tape_;    // L2Queue: trades since the mirror's last update
  std::unique_ptr<MdAggregator> agg_;
  SimObserver* observer_ = nullptr;
  OutboundHasher hasher_;
  SimTransportStats stats_{};
  std::uint64_t next_queue_order_id_ = 1;
  std::uint64_t next_exec_id_ = 1;
};

static_assert(TransportLike<SimTransport>);

}  // namespace fastmm::sim
