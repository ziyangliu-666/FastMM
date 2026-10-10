#pragma once
// Query side of a storage backend: what fastmm-pnl and a restarting session read back.
//
// Rows are strings. The store is queried by people and by tools, never on a hot path, and a
// string column keeps the interface small enough for a backend to implement in an afternoon;
// raw fixed-point columns are rendered as exact decimals (Fixed::to_string).
#include "fastmm/core/fill_audit.hpp"
#include "fastmm/core/result.hpp"
#include "fastmm/store/backend.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace fastmm::store {

// The executions an engine stored for one venue (Reader::booked_fills): the engine's side of a
// fill audit (core/fill_audit.hpp).
struct BookedFillQuery {
  std::string engine;        // [engine] name (empty: every engine)
  std::string venue;         // [venues.<name>] (empty: every venue)
  std::string instrument;    // symbol (empty: every instrument)
  std::int64_t from_ms = 0;  // the venue's time of the trade, inclusive (0: from the beginning)
  std::int64_t to_ms = 0;    // inclusive (0: to the end)
};

struct Rows {
  std::vector<std::string> columns;
  std::vector<std::vector<std::string>> rows;
  [[nodiscard]] bool empty() const noexcept { return rows.empty(); }
};

// Every query takes the same filter; a backend ignores the fields its query has no use for.
// Which session, and when in it, for the parameter queries.
struct ParamQuery {
  std::string engine;            // [engine] name (empty: every engine)
  std::uint64_t session_id = 0;  // 0: the newest session that started by at_ns
  std::int64_t at_ns = 0;        // wall clock; 0: now (the session's last update)
  std::string instrument;        // symbol: its own values and the shared ones (empty: every scope)
};

struct QueryFilter {
  std::string engine;            // [engine] name (empty: every engine)
  std::uint64_t session_id = 0;  // 0: every session
  std::string instrument;        // symbol (empty: every instrument)
  std::string from;              // inclusive UTC day, "YYYY-MM-DD" (empty: from the beginning)
  std::string to;                // inclusive UTC day (empty: to the end)
  std::size_t limit = 0;         // 0: no limit
  std::string order;             // client order id (order_timeline; empty: none)
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

  // The positions a session carries over (fastmm-live restores them before it connects; see
  // Venue::resume_executions): per venue and symbol, the last one the engine's store holds, from
  // this session or, for a venue and symbol it recorded nothing about (it crashed first), the
  // newest earlier session that did. Two venues can list the same symbol.
  struct PositionState {
    std::uint8_t venue_id = 0;  // in the session that recorded it
    std::string venue;          // its [venues.<name>]; empty when the store predates schema 3
    std::string symbol;
    std::int64_t qty_raw = 0;
    std::int64_t avg_px_raw = 0;
  };
  std::vector<PositionState> position_state;
  // The session epochs (the high bits of their client order ids) of the sessions a restart carries
  // over, newest first, at most kMaxSessionEpochs: behind fastmm-gateway, executions of their
  // orders are this engine's, whichever session placed them.
  static constexpr std::size_t kMaxSessionEpochs = 64;
  std::vector<std::uint16_t> session_epochs;
  // The orders of those sessions that were not terminal at their last record and that the venue
  // had acknowledged (a venue order id), newest first, at most kMaxPastOrders. A venue's trade
  // history can name an execution by the venue's order id alone (Binance); behind fastmm-gateway,
  // after a gateway restart, only this store can say whose order that was.
  struct PastOrder {
    std::string venue;  // its [venues.<name>]; empty when the store predates schema 3
    std::string symbol;
    std::string venue_order_id;
    std::string cl_ord_id;
  };
  static constexpr std::size_t kMaxPastOrders = 256;
  std::vector<PastOrder> past_orders;
  // Where each venue's execution replay resumes (Venue::resume_executions): from the venue time of
  // the last fill or funding payment the store holds for it (in the newest session that stored
  // one), minus kResumeOverlapMs, skipping the trade ids and funding ids (kFundingIdPrefix + id)
  // the store holds from there on. The funding replay starts there too, so a payment made while no
  // session ran is booked by the next one. Both ends are the venue's clock, so the engine's clock
  // (which follows the host's and may be seconds off, a WSL2 clock step) does not enter.
  //
  // The ids are those of every session of the engine, not of the session the start comes from: a
  // session killed seconds after it started stores only what its own replay booked, and the next
  // replay's window still reaches the executions the sessions before it stored. An execution the
  // store holds, in whichever session, is never handed to a replay as new.
  //
  // The start is where the record is known to be whole: the last fill stored up to the newest
  // session that got past its own replay on the venue (the store records a reconciliation that
  // began with the venue's executions complete, schema 6; a session from before that counts when it
  // placed an order there - none goes out before the venue has reconciled - or shut down cleanly).
  // A session after that one died while its replay was still reading, and can hold a fill newer
  // than one the replay had not reached: its fills are known, and the start is not later than the
  // earliest of their last ones. When no session at all got past its replay the start is no later
  // than unbooked_since_ms.
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
    std::uint8_t venue_id = 0;  // the session's VenueId
    std::string venue;          // its [venues.<name>]; empty when the store predates schema 3
    // Venue time of the stored fill or funding payment the start is taken from (see above).
    std::int64_t last_fill_ms = 0;
    std::int64_t since_ms = 0;  // the replay's start, venue time, inclusive
    // Stored fills and funding at or after since_ms, of every session of the engine.
    std::vector<std::string> known_exec_ids;
    bool shrunk = false;  // since_ms moved later than last_fill_ms - kResumeOverlapMs
  };
  // One entry per venue that recorded a fill with a venue time.
  std::vector<VenueResume> venue_resume;
  // The highest numeric trade id per (venue, symbol) the store holds up to the newest session that
  // got past its replay on the venue (see above), and the ids above it that the sessions after
  // that one stored. Binance trade ids increase per symbol, so its replay resumes at last_id + 1
  // exactly and skips known_after (Venue::resume_trade_ids). No entry for a venue no session got
  // past its replay on: it replays by time.
  struct TradeIdMark {
    std::uint8_t venue_id = 0;
    std::string venue;  // empty when the store predates schema 3
    std::string symbol;
    std::int64_t last_id = 0;
    std::vector<std::int64_t> known_after;  // ascending
  };
  std::vector<TradeIdMark> last_trade_ids;

  // The fallback for a venue with no entry above (a store from before schema 3, or a venue whose
  // fills carry no venue time): the engine clock, as before. The replay starts kFallbackOverlapNs
  // before the session's last fill, and the ids reach kFallbackIdsNs back, further than the start,
  // because the start is local time and the venue compares it with its own. At most
  // kMaxKnownExecIds, with the start moved later when more fall inside.
  static constexpr std::int64_t kFallbackOverlapNs = 10'000'000'000;  // 10 s
  static constexpr std::int64_t kFallbackIdsNs = 2 * kFallbackOverlapNs;
  std::int64_t last_fill_ns = 0;       // engine time of the newest session's last fill (0: none)
  std::int64_t fallback_since_ms = 0;  // 0: no replay
  std::vector<std::string> fallback_exec_ids;  // of every session that ran inside kFallbackIdsNs

  // Where the replay of a venue with no stored fill starts (no venue_resume entry, in a store
  // whose fills carry the venue's time or that holds none): the start of the newest session that
  // shut down cleanly, else of the oldest session, less kFallbackOverlapNs, engine clock. Whatever
  // the venue executed since is unbooked: a fill of a session that crashed before storing it, or
  // one made while nothing ran.
  std::int64_t unbooked_since_ms = 0;

  // Executions and funding payments the engine's store holds more than once (Reader::duplicates):
  // a restart booked them a second time, so the positions and the PnL of the sessions holding the
  // copies count them twice. Reported, never repaired: the rows stay as they were written. At most
  // kMaxDuplicates are listed; duplicate_count is how many there are.
  struct Duplicate {
    std::string venue;  // its [venues.<name>]; "#<id>" when the store predates schema 3
    std::string symbol;
    std::string id;        // the venue's trade id or funding id
    std::string qty;       // decimal: the execution's quantity, the funding payment's amount
    std::string side;      // empty for a funding payment
    std::string sessions;  // the sessions holding it, oldest first, space separated
    std::uint32_t copies = 0;
    bool funding = false;
  };
  static constexpr std::size_t kMaxDuplicates = 32;
  std::vector<Duplicate> duplicates;
  std::uint64_t duplicate_count = 0;
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
  // One row per venue execution or funding payment stored more than once (by venue, symbol and
  // the venue's id of it, within an engine): what a restart booked a second time. Empty in a
  // sound store; the default has none. Filtered by engine and instrument.
  [[nodiscard]] virtual Result<Rows, std::string> duplicates(const QueryFilter& /*f*/) {
    return Rows{};
  }
  // The economic fill ledger: one row per execution (the venue's id of it on an account, symbol
  // and side, within an engine) however many sessions stored it, as the first one did, with when
  // it was first received, the venue's time, the quantity the position took (booked), how many
  // copies there are and its flags (synthetic: booked from a cum_qty jump, no venue report; late:
  // for an order already ended; before_start: traded before the session that booked it started,
  // so it came in through the restart's replay; repeated: stored more than once). `raw` lists
  // every stored row instead, with its copy number. A session filter keeps the executions it
  // booked first (raw: its rows). The default cannot answer.
  [[nodiscard]] virtual Result<Rows, std::string> ledger(const QueryFilter& /*f*/, bool /*raw*/) {
    return fail(std::string("this store backend has no fill ledger"));
  }
  // Everything stored about one client order (`f.order`), in time order: sent, each refusal, each
  // execution, and its last known state. The default cannot answer.
  [[nodiscard]] virtual Result<Rows, std::string> order_timeline(const QueryFilter& /*f*/) {
    return fail(std::string("this store backend has no order timeline"));
  }
  // The executions stored for `q.venue`, of every session of `q.engine`, by the venue's time of the
  // trade: the venue's trade id, order ids, side, price, quantity and the commission as reported
  // (AuditFill::fee_asset: quote, base or other). Estimates (no trade id) are left out. Needs the
  // venue's names and times (schema 3); the default cannot answer.
  [[nodiscard]] virtual Result<std::vector<AuditFill>, std::string> booked_fills(
      const BookedFillQuery& /*q*/) {
    return fail(std::string("this store backend cannot list booked fills"));
  }
  // The last position snapshot per session and instrument.
  [[nodiscard]] virtual Result<Rows, std::string> positions(const QueryFilter& f) = 0;
  // The strategy parameters in effect at `q.at_ns` (0: the session's last update) in the session
  // `q.session_id`, else the newest of `q.engine` that started by then: one row per parameter and
  // scope (every instrument, or one that has its own value), with when it was set, its origin
  // (initial, control, strategy) and source. Needs schema 8; the default cannot answer.
  [[nodiscard]] virtual Result<Rows, std::string> params(const ParamQuery& /*q*/) {
    return fail(std::string("this store backend keeps no parameter history"));
  }
  // Every parameter update, one row per value it carried, in order: the starting set (origin
  // initial) and each update the engine applied. Filtered by engine, session, day and instrument.
  [[nodiscard]] virtual Result<Rows, std::string> param_changes(const QueryFilter& /*f*/) {
    return fail(std::string("this store backend keeps no parameter history"));
  }
  // The parameters session `a` ended with against those session `b` started with: one row per
  // parameter and scope that differs. `a` 0: the session of b's engine that started last before b
  // (what a restart changed).
  [[nodiscard]] virtual Result<Rows, std::string> param_diff(std::uint64_t /*a*/,
                                                             std::uint64_t /*b*/) {
    return fail(std::string("this store backend keeps no parameter history"));
  }
  // One row per refused order the engine recorded (`count` with the ones folded into it): when,
  // which account, instrument and side, whether it only reduced the position, the reason, the
  // limit or side that refused it (source) and the account's budget then. Filtered by engine,
  // session, day and instrument. Needs schema 9.
  [[nodiscard]] virtual Result<Rows, std::string> rejects(const QueryFilter& /*f*/) {
    return fail(std::string("this store backend keeps no rejects"));
  }
  // The same summed: one row per account, instrument, side, reduces, reason and source.
  [[nodiscard]] virtual Result<Rows, std::string> reject_summary(const QueryFilter& /*f*/) {
    return fail(std::string("this store backend keeps no rejects"));
  }
  // The effective configuration (TOML, secrets omitted) of the session `q` picks as params() does.
  [[nodiscard]] virtual Result<std::string, std::string> session_config(const ParamQuery& /*q*/) {
    return fail(std::string("this store backend keeps no configuration"));
  }
  // The newest session of `f.engine`, summarised.
  [[nodiscard]] virtual Result<Recovery, std::string> recovery(const QueryFilter& f) = 0;
};

}  // namespace fastmm::store
