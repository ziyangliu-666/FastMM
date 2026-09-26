#pragma once
// OwnQuantity: our resting quantity that a venue's public feed shows, by price and venue time.
//
// A live venue's depth and top of book include our own orders. OwnQuantity follows each order's
// life in venue time from the order messages (the same stream the journal records):
//
//   rests from   the ack's exch_ts; a venue that answers twice may put the time on either copy,
//                the first non-zero one is used, and an ack without one falls back to recv_ts
//   steps down   at each fill's venue time, by the fill (fills repeated by exec id count once)
//   rests until  the end's venue time: cancel ack, last fill, expiry, a reconciliation that no
//                longer lists it (or the OMS ending it another way, on_gone); a replaced order
//                until its replacement's ack when that is later; with whole-millisecond order
//                times (Binance) until the end of the end's millisecond
//
// own_at(inst, side, px, t) is the quantity resting at `px` at venue time t, with what is known
// now: a depth update stamped before our cancel still includes the order, one stamped after it
// does not. The engine asks it at the book's last update (StrategyContext::own_qty); the backtest
// strips a live journal's feed with the same object after reading the whole journal
// (backtest/own_orders.hpp, keep_all). Segments older than `retention` in venue time are dropped
// unless keep_all.
//
// Only order events do work here (a few segment updates for the order they name); a book update
// costs nothing. All state derives from journaled messages, so a replay reproduces it.
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/containers/pool.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace fastmm {

// A time of an order event: the venue's (exch_ts), or the receive time when the venue gave none.
struct VenueTime {
  Timestamp ts;
  bool from_recv = true;
  // Takes `h`'s time if there is none yet, or if the one held is only a receive time.
  void offer(const EventHeader& h) noexcept;
};

// Binance's exec id is the public trade id; 0 when the id is not a number.
[[nodiscard]] std::uint64_t numeric_exec_id(const ExecId& id) noexcept;

class OwnQuantity {
 public:
  static constexpr std::size_t kMaxFills = 8;  // per order; more are merged into the last

  struct Stats {
    std::uint64_t orders = 0;        // OutNewOrder + OutReplace seen
    std::uint64_t unknown_acks = 0;  // acks of orders not seen going out
    std::uint64_t evicted = 0;       // unacknowledged orders dropped to make room
    std::uint64_t merged_fills = 0;  // fills past kMaxFills
  };

  // keep_all: never drop a segment (a whole journal, then index()).
  explicit OwnQuantity(bool keep_all = false, Duration retention = seconds(5));
  OwnQuantity(const OwnQuantity&) = delete;
  OwnQuantity& operator=(const OwnQuantity&) = delete;
  ~OwnQuantity();

  // OutNewOrder and OutReplace the session sent (not the ones the transport refused).
  void on_outbound(const EventHeader& h) noexcept;
  // Order events from the venue; other types are ignored.
  void on_inbound(const EventHeader& h) noexcept;
  // The OMS ended the order in a way on_inbound does not see (a cancel the venue rejected because
  // it no longer knows the order): it stops resting at venue time t, or is forgotten if it never
  // rested.
  void on_gone(ClientOrderId id, Timestamp t) noexcept;

  // Makes room for instruments [0, n) so that order events do not allocate (the engine, at start).
  void prepare(std::size_t instruments);

  // Our resting quantity at (inst, side, px) at venue time t.
  [[nodiscard]] Qty own_at(InstrumentId inst, Side side, Price px, Timestamp t) const noexcept;
  // keep_all: sorts the segments for fast own_at over a long journal.
  void index();
  [[nodiscard]] bool empty() const noexcept;
  // Every venue order time so far was a whole millisecond.
  [[nodiscard]] bool ms_order_times() const noexcept { return ms_ && venue_times_ > 0; }
  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

 private:
  struct Fill {
    VenueTime at;
    Qty qty;
    std::uint64_t exec;
  };
  struct Life {
    ClientOrderId id;
    ClientOrderId replaces;
    ClientOrderId replaced_by;
    InstrumentId inst;
    VenueId venue;
    Side side;
    bool resting;  // can rest on the book (not market, IOC or FOK)
    bool acked;
    bool ended;
    bool rejected;
    bool seen;       // listed by the reconciliation in progress
    bool cancelled;  // ended by a cancel ack or by its replacement
    Price px;
    Qty qty;
    VenueTime ack;
    VenueTime end;
    std::uint64_t born;  // order of arrival, for eviction
    std::uint32_t nfills;
    Fill fills[kMaxFills];
  };
  struct Segment {
    std::int64_t start;  // [start, end) in venue ns
    std::int64_t end;
    Qty qty;
    Price px;
    ClientOrderId owner;
    Side side;
  };
  struct Index;

  Life* find(ClientOrderId id) noexcept;
  Life* add(const Life& l) noexcept;
  void drop(ClientOrderId id) noexcept;
  void on_ack(const EventHeader& h, ClientOrderId id) noexcept;
  void on_fill(const EventHeader& h, const OrderFillMsg& m) noexcept;
  void on_cancel_ack(const EventHeader& h, ClientOrderId id) noexcept;
  void on_reject(ClientOrderId id) noexcept;
  void on_reconcile(const EventHeader& h, const ReconcileMsg& m) noexcept;
  void end(Life& s, const EventHeader& h, bool cancelled = false) noexcept;
  [[nodiscard]] Timestamp gone(const Life& s) noexcept;
  void rebuild(Life& s) noexcept;
  void gc(Timestamp now) noexcept;
  [[nodiscard]] std::vector<Segment>& segs(InstrumentId id);

  bool keep_all_;
  Duration retention_;
  bool ms_ = true;
  std::uint64_t venue_times_ = 0;
  std::uint64_t born_ = 0;
  std::int64_t last_gc_ = 0;
  Timestamp latest_;  // latest venue time of an order event
  ClientOrderId watermark_;
  VenueId reconcile_venue_;
  Stats stats_;
  std::unique_ptr<Pool<Life, kMaxOpenOrders>> lives_;
  std::unique_ptr<OpenHashMap<ClientOrderId, Handle<Life>, kMaxOpenOrders * 2>> by_id_;
  std::vector<std::vector<Segment>> by_inst_;  // unsorted; the engine's queries scan one
  std::vector<ClientOrderId> scratch_;         // orders to end or drop, collected before
  std::unique_ptr<Index> index_;               // keep_all, after index()
};

}  // namespace fastmm
