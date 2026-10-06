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
// instrument, or through the pool member its header names (SimTransportConfig::pools: a member is
// a further link with its own account over the primary's books; an order's events come back on the
// link it went out on). The wires model each venue's two TCP streams: messages arrive in order, so
// arrivals are clamped monotone per wire, and a slow venue never holds back a fast one. Books, the
// matching engine and the queue model are shared: instruments never span venues. SimDriver decides
// when to move a wire message into the engine's feed (when the virtual clock reaches its recv_ts);
// on a tie order wires come before md wires, lower venue ids first.
//
// Market data reaches the strategy in one of two ways:
//   * coupled generator: a MarketGenerator drives the MatchingEngine and the MdAggregator
//     publishes Binance-style 100 ms depth batches of the shared book (strategy orders
//     included, fills are real matches);
//   * source data: on_source_event() applies each historical event to the venue-side
//     state (mirror book + queue model, or account-0 liquidity in the matching engine so
//     the book trades through resting strategy orders) and forwards it to the engine. Events
//     from VenueOrderSource come in venue-time order with their recorded position in hdr.seq;
//     the engine still gets them in recorded order: one that came ahead of an earlier-recorded
//     event waits for it before going onto the wire (the venue side has applied it already). With
//     own_orders_in_feed the forwarded feed shows the strategy's resting orders, as a live
//     venue's does: each depth level and book ticker carries our quantity at its price, the next
//     depth update also carries our levels that changed since the last one, and a book ticker
//     goes out when our orders move the top of book (data with book tickers only).
#include "fastmm/core/account_pool.hpp"
#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/transfer.hpp"
#include "fastmm/core/transport.hpp"
#include "fastmm/sim/event_scheduler.hpp"
#include "fastmm/sim/fee_model.hpp"
#include "fastmm/sim/latency_model.hpp"
#include "fastmm/sim/matching_engine.hpp"
#include "fastmm/sim/md_aggregator.hpp"
#include "fastmm/sim/md_source.hpp"
#include "fastmm/sim/outbound_hash.hpp"
#include "fastmm/sim/queue_model.hpp"
#include "fastmm/sim/sim_account.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
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
  // The venue's order-count windows (Binance ORDERS per 10 s and per day, fixed intervals of the
  // clock): a new order or replace past the limit is refused with VenueRateLimit, and
  // ctx.order_budget reports the counts. 0: unlimited and not reported.
  std::int64_t orders_10s = 0;
  std::int64_t orders_1d = 0;
  // Cancels' own engine -> venue latency (cancel_latency set), else the order path's.
  bool cancel_latency = false;
  LatencyParams cancel_out{};
  // The venue takes the order messages of one connection one at a time: a message is processed
  // no sooner than this after the one sent before it (zero: each on its own latency draw).
  Duration order_service{};
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
  std::int64_t orders_10s =
      0;  // SimVenueConfig::orders_10s / orders_1d for venues without an entry
  std::int64_t orders_1d = 0;
  bool cancel_latency = false;  // SimVenueConfig::cancel_latency, cancel_out, order_service
  LatencyParams cancel_out{};
  Duration order_service{};
  MdAggregatorConfig md{};  // coupled-generator mode
  // SHA-256 over every outbound message (outbound_hash()), used by the determinism and replay
  // checks. It costs about 70 ns per order, more than the engine work that produced it, so a
  // benchmark that times send() turns it off; backtests and tests leave it on.
  bool hash_outbound = true;
  std::size_t md_wire_bytes = 4U << 20;
  std::size_t order_wire_bytes = 1U << 20;
  VenueId venue{0};  // venue of the instruments that name none (and of an empty table)
  std::vector<SimVenueConfig> venues;
  // The strategy's account on each venue listed (sim_account.hpp): the venue refuses an order the
  // account cannot cover and reports its balances. A venue not listed takes every order and sends
  // no BalanceMsg.
  std::vector<SimAccountConfig> accounts;
  // Per instrument id, a derivative's initial margin rate ([[instruments]] initial_margin), for the
  // accounts.
  std::vector<Ratio> initial_margin;
  // The account pools ([venues.<x>] pool_of): a member of a simulated venue is simulated too, as
  // a link of its own (its latency from `venues`, its account from `accounts`) over the primary's
  // books.
  PoolPlan pools;
  // The venues' public feed shows the strategy's resting orders (see the top of this file), and
  // the engine follows our quantity in it (own_in_feed(), ctx.own_qty). The coupled generator's
  // book always holds them.
  bool own_orders_in_feed = true;

  // The settings `v` runs with: its entry in `venues`, or the fields above.
  [[nodiscard]] SimVenueConfig venue_config(VenueId v) const noexcept {
    for (const SimVenueConfig& c : venues) {
      if (c.venue == v) return c;
    }
    return SimVenueConfig{v,
                          order_out,
                          ack_in,
                          md_in,
                          md_recorded_arrival,
                          supports_replace,
                          stp,
                          orders_10s,
                          orders_1d,
                          cancel_latency,
                          cancel_out,
                          order_service};
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
  // Rejects broken down by cause; these seven always sum to `rejects`.
  std::uint64_t rejects_post_only = 0;   // PostOnlyWouldCross: crossed the live book on arrival
  std::uint64_t rejects_level_full = 0;  // VenueReject: simulated price-level table full
  std::uint64_t rejects_invalid = 0;     // InvalidTick / InvalidLot / InstrumentDisabled
  std::uint64_t rejects_duplicate = 0;   // DuplicateId
  std::uint64_t rejects_balance = 0;     // InsufficientBalance: the account (sim_account.hpp)
  std::uint64_t rejects_rate_limit = 0;  // VenueRateLimit: the venue's order-count window
  std::uint64_t rejects_other = 0;
  std::uint64_t fills = 0;
  std::uint64_t cancel_acks = 0;
  std::uint64_t cancel_rejects = 0;
  std::uint64_t expired = 0;
  std::uint64_t md_forwarded = 0;
  std::uint64_t md_delivered = 0;
  std::uint64_t own_tickers = 0;  // book tickers sent because our orders moved the top of book
  std::uint64_t own_levels = 0;   // depth levels forwarded with our quantity in them
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
  [[nodiscard]] bool own_in_feed(VenueId v) const noexcept {
    return cfg_.own_orders_in_feed && v.value < kMaxVenues;
  }
  // The venue's order-count windows as of the engine clock (ctx.order_budget): false for a venue
  // without a limit (SimVenueConfig::orders_10s / orders_1d), whose budget stays unknown.
  [[nodiscard]] bool venue_budget(VenueId v, OrderBudget& out) const noexcept;

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
  // Each account's balances as one snapshot per venue, at `now` (SimDriver::start).
  void publish_balances(Timestamp now) noexcept;
  // Historical event at venue time (hdr.exch_ts, falling back to recv_ts): updates the
  // venue-side fill model and forwards the event to the engine after md_in latency, in recorded
  // order when hdr.seq gives it (from 1, VenueOrderSource), else at once.
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
  // An internal transfer between two simulated accounts of one pool, carried out at venue time
  // `ts` (SimAccounts::transfer): each account's new balance goes to the engine on its own link as
  // a BalanceMsg, as a venue's account stream reports one. Done, or Failed when the accounts
  // refuse it.
  TransferState transfer(
      VenueId from, VenueId to, std::string_view asset, Notional amount, Timestamp ts) noexcept;
  // The strategy's accounts; null when no venue has one.
  [[nodiscard]] const SimAccounts* accounts() const noexcept { return accounts_.get(); }
  // The pool member an open order of the strategy went to; invalid for an order the venue does
  // not hold (or without pools).
  [[nodiscard]] VenueId order_venue(ClientOrderId id) const noexcept {
    if (order_venues_ == nullptr) return VenueId{};
    const std::uint8_t* v = order_venues_->find(id.value);
    return v == nullptr ? VenueId{} : VenueId{*v};
  }
  void set_observer(SimObserver* o) noexcept { observer_ = o; }

  // ---- MatchingSink (venue events for account 1 become engine messages) -------------------
  void on_ack(const SimOrder& o, Timestamp ts) override;
  void on_reject(const NewOrder& o, RejectReason r, Timestamp ts) override;
  void on_cancel(const SimOrder& o, CancelReason r, Timestamp ts) override;
  void on_cancel_reject(AccountId a, ClientOrderId id, InstrumentId inst, Timestamp ts) override;
  void on_fill(
      const SimOrder& maker, const SimOrder& taker, Price px, Qty qty, Timestamp now) override;
  void on_book_change(InstrumentId id, Side s, Price px, Qty qty, std::uint64_t uid) override;
  void on_trade(
      InstrumentId id, Price px, Qty qty, Side aggr, std::uint64_t tid, Timestamp ts) override;

 private:
  struct OutSlot {
    std::uint32_t len;
    std::uint8_t over_limit;  // past the venue's order-count window when sent: refused on arrival
    alignas(8) std::byte bytes[kOutSlotBytes];
  };
  // One fixed window of a venue's order count (Binance's intervalNum x interval): orders sent in
  // the interval of the clock that holds `now`.
  struct OrderWindow {
    std::int64_t width_ns = 0;
    std::int64_t limit = 0;  // 0: none
    std::int64_t index = -1;
    std::int64_t count = 0;
    // The window holding `now` is full: an order sent now is refused (and not counted, as
    // Binance's -1015 is not).
    [[nodiscard]] bool full(Timestamp now) noexcept {
      if (limit <= 0) return false;
      roll(now);
      return count >= limit;
    }
    void add(Timestamp now) noexcept {
      if (limit <= 0) return;
      roll(now);
      ++count;
    }
    void roll(Timestamp now) noexcept {
      const std::int64_t i = now.ns / width_ns;
      if (i != index) {
        index = i;
        count = 0;
      }
    }
    [[nodiscard]] std::int64_t used_at(Timestamp now) const noexcept {
      return limit > 0 && now.ns / width_ns == index ? count : 0;
    }
  };
  // One simulated venue: its latency model and its two wires to the engine.
  struct Link {
    Link(VenueId v, const SimVenueConfig& c, std::uint64_t seed, const SimTransportConfig& t)
        : id(v),
          lat(c.cancel_latency ? LatencyModel(c.order_out, c.ack_in, c.md_in, c.cancel_out, seed)
                               : LatencyModel(c.order_out, c.ack_in, c.md_in, seed)),
          md_wire(t.md_wire_bytes),
          order_wire(t.order_wire_bytes),
          orders_10s{seconds(10).ns, c.orders_10s},
          orders_1d{seconds(86'400).ns, c.orders_1d},
          md_recorded_arrival(c.md_recorded_arrival),
          order_service(c.order_service) {}
    VenueId id;
    LatencyModel lat;
    MsgRing md_wire;
    MsgRing order_wire;
    Timestamp last_md_arrival{};
    Timestamp last_order_arrival{};
    OrderWindow orders_10s;  // SimVenueConfig::orders_10s / orders_1d
    OrderWindow orders_1d;
    std::uint64_t taken = 0;  // new orders and replaces sent (OrderBudget::orders_taken)
    bool md_recorded_arrival;
    Duration order_service;          // SimVenueConfig::order_service
    Timestamp last_order_processed;  // venue time the last order message was taken in
    [[nodiscard]] bool limited() const noexcept {
      return orders_10s.limit > 0 || orders_1d.limit > 0;
    }
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
  // The balances of the account on `l`'s venue that moved, right behind the order event at
  // `venue_ts` that moved them (the same connection: no latency draw of their own).
  void publish_account(Link& l, Timestamp venue_ts) noexcept;
  void push_balance(Link& l, BalanceMsg& m) noexcept;
  void push_md_wire(EventHeader& h, Timestamp venue_ts) noexcept;
  // A recorded market-data event onto its venue's wire; `recorded`: its recv_ts, for md_arrival
  // = recorded.
  void forward(const EventHeader& md, Timestamp recorded, Timestamp now) noexcept;
  // forward() in recorded order: the event at position `seq` (`md` null: nothing to send) goes
  // out once every earlier position has, those after it that were waiting with it.
  void forward_in_order(const EventHeader* md,
                        std::uint64_t seq,
                        Timestamp recorded,
                        Timestamp now) noexcept;
  // Our orders in the recorded feed (own_orders_in_feed): note_own() marks a level of ours that
  // changed, with_own() gives a recorded depth update or ticker with our quantity in it, and
  // flush_own() sends a ticker for each instrument whose top of book our orders moved.
  void note_own(InstrumentId id, Side side, Price px) noexcept;
  [[nodiscard]] const EventHeader& with_own(const EventHeader& md) noexcept;
  void flush_own(Timestamp now) noexcept;
  // Our resting quantity at a price, from the fill model.
  [[nodiscard]] Qty model_own_at(InstrumentId id, Side side, Price px) const noexcept;
  // The recorded top of book on `side`: the last recorded ticker when it is newer than the depth,
  // else the mirror's.
  [[nodiscard]] Level recorded_top(InstrumentId id, Side side) const noexcept;
  // Our quantity at `px` that the feed shows: none at or through the recorded opposite touch (a
  // live venue would have matched the order there; the fill model waits for a trade).
  [[nodiscard]] Qty shown_own(InstrumentId id, Side side, Price px) const noexcept;
  // The top of book the feed shows now on `side`: the recorded one with our best order shown.
  [[nodiscard]] Level shown_top(InstrumentId id, Side side) const noexcept;
  // Venue of an instrument; an unknown one (a cancel of an order the venue never saw) goes
  // through the first venue.
  [[nodiscard]] Link& link(InstrumentId id) noexcept {
    return id.value < kMaxInstruments ? *link_of_inst_[id.value] : at(0);
  }
  // The link an outbound message takes: the pool member its header names, else its instrument's.
  [[nodiscard]] Link& link_for(const EventHeader& m) noexcept {
    if (order_venues_ != nullptr) {
      if (Link* l = link_of_venue(m.venue); l != nullptr) return *l;
    }
    return link(m.instrument);
  }
  // The link an order's events go back on: the one it came in on (order_venues_), else its
  // instrument's.
  [[nodiscard]] Link& order_link(ClientOrderId id, InstrumentId inst) noexcept {
    if (order_venues_ != nullptr) {
      if (const std::uint8_t* v = order_venues_->find(id.value); v != nullptr) {
        if (Link* l = link_of_venue(VenueId{*v}); l != nullptr) return *l;
      }
    }
    return link(inst);
  }
  // An order arrived on `l` (pools only); forget_order() once it is over.
  void note_order(ClientOrderId id, const Link& l) noexcept {
    if (order_venues_ != nullptr) static_cast<void>(order_venues_->assign(id.value, l.id.value));
  }
  void forget_order(ClientOrderId id) noexcept {
    if (order_venues_ != nullptr) static_cast<void>(order_venues_->erase(id.value));
  }
  [[nodiscard]] Link* link_of_venue(VenueId v) noexcept {
    for (std::size_t k = 0; k < n_links_; ++k) {
      if (at(k).id == v) return &at(k);
    }
    return nullptr;
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
  std::unique_ptr<SimAccounts> accounts_;
  // With pools: the strategy's orders at the venue by client id -> the member link's venue id.
  using OrderVenues = OpenHashMap<std::uint64_t, std::uint8_t, SimAccounts::kMaxOrders>;
  std::unique_ptr<OrderVenues> order_venues_;
  // Per instrument, recorded data with own_orders_in_feed (null otherwise and in coupled mode).
  struct OwnFeed {
    std::vector<Level> levels[2];               // our resting quantity by price, per Side
    std::vector<std::pair<Side, Price>> stale;  // levels of `levels` to recompute
    std::vector<std::pair<Side, Price>> dirty;  // our levels changed since the last depth update
    bool pending = false;                       // our orders changed since the last flush_own()
    bool tickers = false;                       // the recorded feed has book tickers
    bool ticker_newer = false;                  // the last recorded top came in a ticker
    Level recorded_bid{};                       // the last recorded ticker's top
    Level recorded_ask{};
    Level sent_bid{};  // the top of the last ticker forwarded
    Level sent_ask{};
  };
  // Brings `f.levels` up to date with the fill model.
  void refresh_own(InstrumentId id, OwnFeed& f) noexcept;
  std::unique_ptr<OwnFeed[]> own_feed_;
  // Recorded events applied ahead of an earlier-recorded one, waiting to be forwarded
  // (forward_in_order): a min-heap on the position, their bytes in `waiting_bytes_`.
  struct Waiting {
    std::uint64_t seq;
    Timestamp recorded;
    Timestamp venue;
    std::uint32_t slot;  // kNoSlot: nothing to forward
  };
  static constexpr std::uint32_t kNoSlot = 0xFFFF'FFFF;
  std::vector<Waiting> waiting_;
  std::vector<std::vector<std::uint64_t>> waiting_bytes_;
  std::vector<std::uint32_t> waiting_free_;
  std::uint64_t next_forward_ = 1;
  std::vector<InstrumentId> own_pending_;
  std::unique_ptr<EventBuf> own_buf_;
  SimObserver* observer_ = nullptr;
  OutboundHasher hasher_;
  SimTransportStats stats_{};
  std::uint64_t next_queue_order_id_ = 1;
  std::uint64_t next_exec_id_ = 1;
};

static_assert(TransportLike<SimTransport>);

}  // namespace fastmm::sim
