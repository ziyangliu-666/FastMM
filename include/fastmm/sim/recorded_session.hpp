#pragma once
// RecordedSession: what a recorded session's journal says about the session itself, besides its
// market data, for a backtest over it to read back ([backtest] initial_state = "journal",
// start_at, params_from_journal, balances_from_journal). Everything here is in the journal as it
// happened: the states the session restored and checkpointed, the positions it reconciled before
// its first order, the parameter updates it received, and the account as its venues reported it
// (every balance row and every flow of money in or out). A journal source fills it once when it
// opens the journal (backtest/journal_source.hpp); the backtest reads it before the run.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/sim/sim_account.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fastmm::sim {

// One state the session wrote (StrategyStateMsg): the bytes it handed to restore() at the start, or
// a checkpoint ([engine] checkpoint_s, a take-over). `positions` are Reconcile Position messages on
// the account whose report would set them, `orders` the open orders (format v4).
struct Checkpoint {
  Timestamp at;           // the engine time it was written at
  bool accepted = false;  // a restored state the strategy took
  std::string strategy;   // the strategy's bytes (none for a strategy that keeps no state)
  std::vector<ReconcileMsg> positions;
  std::vector<StateOrder> orders;
};

// What a recorded session started from (backtest [backtest] initial_state = "journal"): the
// bytes for the strategy's restore(), and Reconcile Position messages that set the engine's
// positions as the session had them. `at` is the checkpoint's time (invalid: the session's start).
// `note` says where they came from.
struct SessionStart {
  bool has_state = false;  // `strategy` holds a recorded state (possibly empty)
  std::string strategy;
  std::vector<ReconcileMsg> positions;
  Timestamp at{};
  std::string note;
};

// A recorded session's parameter updates (backtest [backtest] params_from_journal): each at the
// time the session received it (hdr.recv_ts), its fields by the session's parameter table, the
// name and ParamType of each recorded index (empty: the indices are the strategy's own).
struct RecordedParams {
  std::vector<std::pair<std::string, std::uint8_t>> table;
  std::vector<ParamUpdateMsg> updates;
};

struct RecordedSession {
  std::optional<Checkpoint> restored;   // the first restored state
  std::vector<Checkpoint> checkpoints;  // in journal order
  // Reconcile Position before the first order, the latest per instrument and account: what the
  // session restored from its store and what its venues reported at the start.
  std::vector<ReconcileMsg> reconciled;
  RecordedParams params;
  // The account rows (BalanceMsg without kAccount) and the flows (AccountFlowMsg), in journal
  // order.
  std::vector<BalanceMsg> balances;
  std::vector<AccountFlowMsg> flows;

  // What initial_state = "journal" starts from. Without `at`: the restored state, else the first
  // checkpoint, with that checkpoint's positions or the reconciled ones. With `at`: the last
  // checkpoint written at or before it; nothing (has_state false, no positions) when there is none.
  [[nodiscard]] SessionStart start(Timestamp at = {}) const;
  // Each venue's account as it last reported it at or before `at` (every row of a snapshot, then
  // the single rows after it; an asset a snapshot did not name holds nothing), each asset at its
  // total; a venue with no report by then has its first snapshot. Without `at`: every venue's
  // first snapshot.
  [[nodiscard]] std::vector<SimAccountConfig> balances_at(Timestamp at = {}) const;
  // When each venue's account was reported whole (the last row of a snapshot), in journal order:
  // a live connector's account refreshes.
  [[nodiscard]] std::vector<std::pair<VenueId, Timestamp>> refreshes() const;
};

// The venue's time of a balance row or a flow, else the time it was received.
[[nodiscard]] inline Timestamp account_time(const EventHeader& h) noexcept {
  return h.exch_ts.valid() ? h.exch_ts : h.recv_ts;
}

}  // namespace fastmm::sim
