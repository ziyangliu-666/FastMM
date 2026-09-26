#pragma once
// Query side of a storage backend: what fastmm-pnl and a restarting session read back.
//
// Rows are strings. The store is queried by people and by tools, never on a hot path, and a
// string column keeps the interface small enough for a backend to implement in an afternoon;
// raw fixed-point columns are rendered as exact decimals (Fixed::to_string).
#include "fastmm/core/result.hpp"
#include "fastmm/store/backend.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace fastmm::store {

struct Rows {
  std::vector<std::string> columns;
  std::vector<std::vector<std::string>> rows;
  [[nodiscard]] bool empty() const noexcept { return rows.empty(); }
};

// Every query takes the same filter; a backend ignores the fields its query has no use for.
struct QueryFilter {
  std::string engine;            // [engine] name (empty: every engine)
  std::uint64_t session_id = 0;  // 0: every session
  std::string instrument;        // symbol (empty: every instrument)
  std::string from;              // inclusive UTC day, "YYYY-MM-DD" (empty: from the beginning)
  std::string to;                // inclusive UTC day (empty: to the end)
  std::size_t limit = 0;         // 0: no limit
};

// What the previous session left behind, for the operator who is about to start a new one.
struct Recovery {
  bool found = false;
  std::uint64_t session_id = 0;
  std::string started_utc;
  std::string stopped_utc;  // empty: the session never recorded a close (it was killed)
  std::string strategy;
  std::string kill_reason;
  bool kill_latched = false;
  bool clean_shutdown = false;
  int exit_code = 0;
  std::string realized;  // decimal, settlement currency
  std::string funding;   // the part of realized that is funding (schema 4; "0" before)
  std::string unrealized;
  std::string fees;
  std::string net;
  std::uint64_t fills = 0;
  std::uint64_t records_dropped = 0;
  bool journal_complete = true;
  std::vector<std::string> journals;
  // "SYMBOL qty @ avg_px realized=... fees=..." per instrument with a position or a fill.
  std::vector<std::string> positions;
  // "cl_ord_id SYMBOL side qty @ price state" per order that was not terminal at the last record.
  std::vector<std::string> open_orders;

  // The same positions as numbers, for a session that carries them over (fastmm-live restores them
  // before it connects; see Venue::resume_executions).
  struct PositionState {
    std::string symbol;
    std::int64_t qty_raw = 0;
    std::int64_t avg_px_raw = 0;
  };
  std::vector<PositionState> position_state;
  // Where each venue's execution replay resumes (Venue::resume_executions): from the venue time of
  // the last fill or funding payment the store holds for it, minus kResumeOverlapMs, skipping the
  // trade ids and funding ids (kFundingIdPrefix + id) the store holds from there on. The funding
  // replay starts there too, so a payment made while no session ran is booked by the next one. Both
  // ends are the venue's clock, so the engine's clock (which follows the host's and may be seconds
  // off, a WSL2 clock step) does not enter.
  //
  // The overlap covers one thing: the order in which a venue publishes executions against their
  // trade times. Executions of different symbols (and a Bybit batch, a Deribit per-instrument
  // channel) can reach the store out of trade-time order; a restart must not skip one that traded
  // just before the last stored fill but had not arrived when the process stopped. That spread is
  // milliseconds to a few hundred; 1 s covers it with margin.
  static constexpr std::int64_t kResumeOverlapMs = 1000;
  // The most trade ids a venue's resume carries (the gateway's attach request holds
  // gw::kMaxKnownExecIds over all its venues). A venue with more stored fills inside the overlap
  // has its start moved later, past the oldest millisecond that does not fit whole, and
  // VenueResume::shrunk says so: dropping an id instead would book that execution twice.
  static constexpr std::size_t kMaxKnownExecIds = 128;
  struct VenueResume {
    std::uint8_t venue_id = 0;      // the session's VenueId
    std::string venue;              // its [venues.<name>]; empty when the store predates schema 3
    std::int64_t last_fill_ms = 0;  // venue time of the last stored fill or funding payment
    std::int64_t since_ms = 0;      // the replay's start, venue time, inclusive
    std::vector<std::string> known_exec_ids;  // stored fills and funding at or after since_ms
    bool shrunk = false;  // since_ms moved later than last_fill_ms - kResumeOverlapMs
  };
  // One entry per venue that recorded a fill with a venue time.
  std::vector<VenueResume> venue_resume;
  // The highest numeric trade id the store holds per (venue, symbol). Binance trade ids increase
  // per symbol, so its replay can resume at the next one exactly (Venue::resume_trade_ids).
  struct TradeIdMark {
    std::uint8_t venue_id = 0;
    std::string venue;  // empty when the store predates schema 3
    std::string symbol;
    std::int64_t last_id = 0;
  };
  std::vector<TradeIdMark> last_trade_ids;

  // The fallback for a venue with no entry above (a store from before schema 3, or a venue whose
  // fills carry no venue time): the engine clock, as before. The replay starts kFallbackOverlapNs
  // before the session's last fill, and the ids reach kFallbackIdsNs back, further than the start,
  // because the start is local time and the venue compares it with its own. At most
  // kMaxKnownExecIds, with the start moved later when more fall inside.
  static constexpr std::int64_t kFallbackOverlapNs = 10'000'000'000;  // 10 s
  static constexpr std::int64_t kFallbackIdsNs = 2 * kFallbackOverlapNs;
  std::int64_t last_fill_ns = 0;       // engine time of the session's last fill (0: none)
  std::int64_t fallback_since_ms = 0;  // 0: no replay
  std::vector<std::string> fallback_exec_ids;
};

class Reader {
 public:
  Reader() = default;
  virtual ~Reader() = default;
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  [[nodiscard]] virtual Result<void, std::string> open(const BackendOptions& opts) = 0;
  // One row per session: id, engine, strategy, start, stop, PnL, fills, exit.
  [[nodiscard]] virtual Result<Rows, std::string> sessions(const QueryFilter& f) = 0;
  // One row per execution.
  [[nodiscard]] virtual Result<Rows, std::string> fills(const QueryFilter& f) = 0;
  // One row per order, in its last known state.
  [[nodiscard]] virtual Result<Rows, std::string> orders(const QueryFilter& f) = 0;
  // One row per UTC day and instrument: realised, fees, unrealised, position, settlement currency.
  [[nodiscard]] virtual Result<Rows, std::string> pnl(const QueryFilter& f) = 0;
  // One row per funding payment. The default has none (a backend without the table).
  [[nodiscard]] virtual Result<Rows, std::string> funding(const QueryFilter& /*f*/) {
    return Rows{};
  }
  // The last position snapshot per session and instrument.
  [[nodiscard]] virtual Result<Rows, std::string> positions(const QueryFilter& f) = 0;
  // The newest session of `f.engine`, summarised.
  [[nodiscard]] virtual Result<Recovery, std::string> recovery(const QueryFilter& f) = 0;
};

}  // namespace fastmm::store
