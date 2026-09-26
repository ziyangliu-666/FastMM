#pragma once
// Our own orders in a recorded session, in venue time, and what they did to the public market data.
//
// collect_own_orders() reads a journal once, in recorded order, and returns every order the session
// sent with its life at the venue: the ack's exch_ts (the matching engine's time), the end's
// (cancel ack, last fill, expiry; a reconciliation that no longer lists it) and its fills. A venue
// that answers twice (API response and user stream) may put the time on either copy; the first
// non-zero one is used, and an event without one falls back to its receive time (from_recv).
//
// OwnOrderStripper takes our resting quantity out of the public depth and top of book of a live
// session: the venue's feed shows our orders, a backtest over that feed would otherwise see them as
// someone else's liquidity. Each level is stripped of what we had resting at that price at the
// message's venue time, so a throttled depth update that still shows an order we have since
// cancelled keeps the quantity it had when we stripped it (none of ours).
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/sim/md_source.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fastmm::bt {

// How the live order left the book.
enum class OrderEnd : std::uint8_t {
  Open = 0,      // still resting when the journal ends
  Canceled = 1,  // cancel ack
  Filled = 2,    // last fill
  Expired = 3,
  Replaced = 4,   // cancel ack of a replaced order; the new id is its own order
  Reconciled = 5  // missing from a reconciliation snapshot
};
[[nodiscard]] std::string_view to_string(OrderEnd e) noexcept;

// Venue time of a market-data message: its exch_ts, else its receive time.
[[nodiscard]] inline Timestamp venue_ts(const EventHeader& h) noexcept {
  return h.exch_ts.valid() ? h.exch_ts : h.recv_ts;
}

// A time of an order event: the venue's (exch_ts), or the receive time when the venue gave none.
struct VenueTime {
  Timestamp ts;
  bool from_recv = true;
  // Takes `h`'s time if there is none yet, or if the one held is only a receive time.
  void offer(const EventHeader& h) noexcept;
};

struct OwnFill {
  VenueTime at;
  Qty qty;
  std::uint64_t exec = 0;  // numeric exec id (Binance: the public trade id), 0 if not a number
};

struct OwnOrder {
  ClientOrderId id;
  InstrumentId instrument;
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  TimeInForce tif = TimeInForce::Gtc;
  Price price;
  Qty qty;
  ClientOrderId replaces;     // OutReplace: the order it replaces
  ClientOrderId replaced_by;  // the OutReplace that replaces it
  bool acked = false;
  bool rejected = false;
  bool ended = false;  // gone at the venue (before or after the ack)
  VenueTime ack;
  VenueTime end;
  OrderEnd why = OrderEnd::Open;
  std::vector<OwnFill> fills;
  std::uint64_t last_exec = 0;  // exec id of the last fill
  // Could rest on the book (not market, IOC or FOK).
  [[nodiscard]] bool resting_type() const noexcept {
    return type != OrderType::Market && tif != TimeInForce::Ioc && tif != TimeInForce::Fok;
  }
};

struct OwnOrderLog {
  std::vector<OwnOrder> orders;  // in the order they were sent
  std::uint64_t orders_sent = 0;
  std::uint64_t rejected = 0;
  std::uint64_t unknown_acks = 0;  // acks of orders the journal has no outbound copy of
  std::uint64_t order_venue_times = 0;
  // Every venue order time is a whole millisecond (Binance transactTime): an order may have
  // entered or left anywhere in that millisecond.
  bool ms_order_times = false;
  // The journal carries a TSC calibration: recorded by a live session, whose venue feed shows our
  // own orders. A backtest's simulated feed does not.
  bool live = false;
  Timestamp last_ts;  // latest venue time of any event

  [[nodiscard]] const OwnOrder* find(ClientOrderId id) const noexcept;
  // When the order stopped resting at the venue: its end, or its successor's ack when a replace
  // took its place later, plus the rest of the end's millisecond with ms_order_times. Invalid
  // while it is still open.
  [[nodiscard]] Timestamp gone(const OwnOrder& o) const noexcept;

  std::unordered_map<std::uint64_t, std::size_t> index;
};

// Reads the whole journal (in recorded order) and resets it.
[[nodiscard]] OwnOrderLog collect_own_orders(JournalReader& reader);

class OwnOrderStripper {
 public:
  struct Stats {
    std::uint64_t levels_adjusted = 0;  // book levels reduced by our quantity
    std::uint64_t levels_removed = 0;   // book levels that were only ours
    std::uint64_t tickers_adjusted = 0;
    std::uint64_t tickers_dropped = 0;  // a side of the ticker was only ours
  };

  explicit OwnOrderStripper(const OwnOrderLog& log);

  // Our resting quantity at (instrument, side, price) at venue time t.
  [[nodiscard]] Qty own_at(InstrumentId inst, Side side, Price px, Timestamp t) const noexcept;

  // `h` with our quantity taken out: `h` itself when nothing changes, a copy in `buf`, or nullptr
  // for a BookTicker whose best bid or ask was only ours (the ticker alone cannot tell the next
  // level; the depth book carries the top instead). Trades and other messages pass unchanged.
  const EventHeader* strip(const EventHeader& h, sim::EventBuf& buf) noexcept;

  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

 private:
  struct Segment {
    std::int64_t start;  // [start, end) in venue ns
    std::int64_t end;
    Qty qty;
  };
  struct Key {
    std::uint32_t inst;
    std::uint8_t side;
    std::int64_t px;
    bool operator==(const Key&) const noexcept = default;
  };
  struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept;
  };
  struct Slot {
    std::vector<Segment> segs;       // by start
    std::vector<std::int64_t> ends;  // running max of segs[0..i].end
  };
  std::unordered_map<Key, Slot, KeyHash> levels_;
  Stats stats_;
};

}  // namespace fastmm::bt
