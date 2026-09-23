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
  // The last position snapshot per session and instrument.
  [[nodiscard]] virtual Result<Rows, std::string> positions(const QueryFilter& f) = 0;
  // The newest session of `f.engine`, summarised.
  [[nodiscard]] virtual Result<Recovery, std::string> recovery(const QueryFilter& f) = 0;
};

}  // namespace fastmm::store
