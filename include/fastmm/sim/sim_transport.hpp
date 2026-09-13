#pragma once
// SimTransport (8.2): the TransportLike that stands in for a venue during backtests.
//
//   engine --send(Out*)--> EventScheduler (now + order_out) --> venue side at fire_ts:
//       FillModel::Matching : MatchingEngine, account 1 (strategy) vs account 0 (flow)
//       FillModel::L2Queue  : QueuePositionModel against a mirror of the historical book
//   venue --acks/fills--> order wire (arrival = max(prev, t + ack_in)) --> InlineFeed
//   venue --market data--> md wire   (arrival = max(prev, t + md_in))  --> InlineFeed
//
// Two byte FIFOs ("wires") model the venue's two TCP streams: messages arrive in order,
// so arrivals are clamped monotone. SimDriver decides when to move a wire message into the
// engine's feed (when the virtual clock reaches its recv_ts).
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
#include <span>

namespace fastmm::sim {

enum class FillModel : std::uint8_t { Matching = 0, L2Queue = 1 };
[[nodiscard]] constexpr std::string_view to_string(FillModel m) noexcept {
  return m == FillModel::Matching ? "matching" : "l2_queue";
}

struct SimTransportConfig {
  LatencyParams order_out{microseconds(200), microseconds(50)};
  LatencyParams ack_in{microseconds(200), microseconds(50)};
  LatencyParams md_in{};
  std::uint64_t seed = 1;
  FillModel fill_model = FillModel::Matching;
  std::int64_t queue_conservatism_bps = 10'000;
  FeeModel fees{};
  bool supports_replace = false;
  StpMode stp = StpMode::None;  // applied to the strategy account
  MdAggregatorConfig md{};      // coupled-generator mode
  std::size_t md_wire_bytes = 4U << 20;
  std::size_t order_wire_bytes = 1U << 20;
  VenueId venue{0};
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
  std::uint64_t fills = 0;
  std::uint64_t cancel_acks = 0;
  std::uint64_t cancel_rejects = 0;
  std::uint64_t expired = 0;
  std::uint64_t md_forwarded = 0;
  std::uint64_t md_delivered = 0;
  std::uint64_t order_events_delivered = 0;
  Notional fees_charged{};
};

// Result-side hooks (backtest collectors). Called on the venue side, in virtual time.
class SimObserver {
 public:
  virtual ~SimObserver() = default;
  // `venue_ts` is the scheduled arrival at the venue; invalid (0) when the latency model
  // dropped the message or the scheduler was full.
  virtual void on_order_sent(const EventHeader&, Timestamp /*send_ts*/, Timestamp /*venue_ts*/) {}
  virtual void on_fill(const OrderFillMsg&, Timestamp /*venue_ts*/, Price /*venue mid*/) {}
  virtual void on_order_event(const EventHeader&, Timestamp /*venue_ts*/) {}
};

class SimTransport final : public MatchingSink {
 public:
  static constexpr std::size_t kSchedulerCapacity = 1U << 14;
  static constexpr std::uint32_t kOutSlotBytes = 192;  // largest Out*Msg

  SimTransport(const SimClock& clock,
               const InstrumentTable& instruments,
               const SimTransportConfig& cfg);

  // ---- TransportLike ------------------------------------------------------------------------
  [[nodiscard]] bool send(const EventHeader& m) noexcept;
  [[nodiscard]] std::size_t send(std::span<const EventHeader* const> batch) noexcept;
  [[nodiscard]] bool supports_replace(VenueId) const noexcept { return cfg_.supports_replace; }

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
  [[nodiscard]] MatchingEngine::SideExposure strategy_exposure(InstrumentId id,
                                                               Side side) const noexcept;
  [[nodiscard]] const SimTransportStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const SimTransportConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] LatencyModel& latency() noexcept { return lat_; }
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
  using Scheduler = EventScheduler<OutSlot, kSchedulerCapacity>;

  // venue-side order handling
  void venue_new(const OutNewOrderMsg& m, Timestamp now) noexcept;
  void venue_cancel(const OutCancelMsg& m, Timestamp now) noexcept;
  void venue_replace(const OutReplaceMsg& m, Timestamp now) noexcept;
  // L2Queue fill model
  void queue_new(const NewOrder& n, Timestamp now) noexcept;
  void queue_cancel(ClientOrderId id, Timestamp now, CancelReason why) noexcept;
  void queue_replace(const OutReplaceMsg& m, Timestamp now) noexcept;
  void queue_on_delta(const BookDeltaMsg& d, Timestamp now) noexcept;
  void queue_on_trade(const TradeMsg& t, Timestamp now) noexcept;
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
  void emit_cancel_reject(ClientOrderId id, InstrumentId inst, Timestamp ts) noexcept;
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
                 Timestamp ts) noexcept;
  void push_order_wire(EventHeader& h, Timestamp venue_ts) noexcept;
  void push_md_wire(EventHeader& h, Timestamp venue_ts) noexcept;
  static void emit_md_thunk(void* ctx, EventHeader& m, Timestamp venue_ts) noexcept;
  [[nodiscard]] static Timestamp head_ts(MsgRing& ring) noexcept;
  bool move_head(MsgRing& ring, InlineFeed& feed, EventType& type) noexcept;

  const SimClock& clock_;
  const InstrumentTable& instruments_;
  SimTransportConfig cfg_;
  MatchingEngine me_;
  LatencyModel lat_;
  Scheduler sched_;
  MsgRing md_wire_;
  MsgRing order_wire_;
  std::unique_ptr<L2Book<256>[]> mirror_;
  QueuePositionModel queue_;
  std::unique_ptr<MdAggregator> agg_;
  SimObserver* observer_ = nullptr;
  OutboundHasher hasher_;
  SimTransportStats stats_{};
  Timestamp last_md_arrival_{};
  Timestamp last_order_arrival_{};
  std::uint64_t next_queue_order_id_ = 1;
  std::uint64_t next_exec_id_ = 1;
};

static_assert(TransportLike<SimTransport>);

}  // namespace fastmm::sim
