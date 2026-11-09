#pragma once
// RecordedSession: what a recorded session's journal says about the session itself, besides its
// market data, for a backtest over it to read back ([backtest] initial_state = "journal",
// start_at, params_from_journal, balances_from_journal, latency_from_journal). Everything here is
// in the journal as it happened: the states the session restored and checkpointed, the positions it
// reconciled before its first order, the parameter updates it received, the account as its venues
// reported it (every balance row and every flow of money in or out), and how long its own messages
// took to and from the venues. A journal source fills it once when it
// opens the journal (backtest/journal_source.hpp); the backtest reads it before the run.
#include "fastmm/core/fx.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/treasury.hpp"
#include "fastmm/sim/recorded_latency.hpp"
#include "fastmm/sim/sim_account.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace fastmm::sim {

// One state the session wrote (StrategyStateMsg): the bytes it handed to restore() at the start, or
// a checkpoint ([engine] checkpoint_s, a take-over). `positions` are Reconcile Position messages on
// the account whose report would set them, `orders` the open orders (format v4).
struct Checkpoint {
  Timestamp at;                // the engine time it was written at
  std::uint32_t version = 0;   // the strategy's state_version when it wrote the bytes
  bool accepted = false;       // a restored state the strategy took
  bool before_orders = false;  // written before the session's first order
  std::string strategy;        // the strategy's bytes (none for a strategy that keeps no state)
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
  std::uint32_t version = 0;  // its state_version
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
  // The journal starts at a later part of its session: the parameter updates of the parts before
  // it are not here, and the configuration is the one the session started with.
  bool starts_mid_session = false;
  // The account rows (BalanceMsg without kAccount) and the flows (AccountFlowMsg), in journal
  // order.
  std::vector<BalanceMsg> balances;
  std::vector<AccountFlowMsg> flows;
  // Replayable Control records in journal order, each as recorded: a command's payload follows its
  // ControlMsg.
  std::vector<std::vector<std::uint64_t>> controls;
  // How long the session's own messages took on each path (latency_from_journal).
  RecordedLatency latency;
  // The journal holds the engine's Start record: it begins where the session began.
  bool from_session_start = false;
  // The journal is still being written: the lists above hold what is known at the start of the run,
  // and the rest reaches the run in its stream (backtest/follow_source.hpp).
  bool growing = false;

  // What initial_state = "journal" starts from. Without `at`: the restored state, else the first
  // checkpoint, with that checkpoint's positions or the reconciled ones. A journal that begins at
  // the session's start does not reach ahead for it: only a checkpoint from before the first order
  // (a warm standby's take-over) is what the session started from. With `at`: the last checkpoint
  // written at or before it; nothing (has_state false, no positions) when there is none.
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

// The operator commands a run replays as they reached the session ([backtest]
// controls_from_journal): quotes pulled and resumed, a flatten, limits changed, a kill switch
// tripped or reset, reduce-only mode switched, a pool home moved by the operator or the planner
// (the engine's own home changes are records, kSynthetic, and the run makes them again), a pool's
// sweep turned on, off or paused. A control
// socket's Stop ends the session and with it the data; what else the engine takes on this path
// concerns the process, not the trading.
[[nodiscard]] constexpr bool replayable_control(ControlCommand c) noexcept {
  switch (c) {
    case ControlCommand::PullQuotes:
    case ControlCommand::ResumeQuotes:
    case ControlCommand::Flatten:
    case ControlCommand::SetLimits:
    case ControlCommand::SetUnderlyingLimit:
    case ControlCommand::TripKill:
    case ControlCommand::TripVenueKill:
    case ControlCommand::ResetKill:
    case ControlCommand::ResetLossBudget:
    case ControlCommand::ReduceOnly:
    case ControlCommand::PoolHome:
    case ControlCommand::PoolSweep:
      return true;
    default:
      return false;
  }
}

// The venue's time of a balance row or a flow, else the time it was received.
[[nodiscard]] inline Timestamp account_time(const EventHeader& h) noexcept {
  return h.exch_ts.valid() ? h.exch_ts : h.recv_ts;
}

// The assets a run moves itself between the accounts of one pool: a simulated treasury's asset, or
// the base assets of the spot instruments of a pool whose sweep the run simulates.
struct TransferScope {
  PoolMembers members;
  std::vector<FixedString<8>> assets;
};

// Two recorded flows that are one transfer between two accounts of a pool a treasury of
// `treasuries` keeps, or within a `scopes` entry: the same amount of its asset out of one account
// and into the other, within kTransferPair. A backtest that simulates the transfers leaves such
// pairs out.
inline constexpr Duration kTransferPair = seconds(60);
[[nodiscard]] inline bool pool_transfer(const AccountFlowMsg& a,
                                        const AccountFlowMsg& b,
                                        std::span<const TreasuryConfig> treasuries,
                                        std::span<const TransferScope> scopes = {}) noexcept {
  if (a.hdr.venue == b.hdr.venue || a.amount.raw != -b.amount.raw || a.amount.raw == 0 ||
      !same_currency(a.asset.view(), b.asset.view()))
    return false;
  const std::int64_t apart = account_time(a.hdr).ns - account_time(b.hdr).ns;
  if (apart > kTransferPair.ns || -apart > kTransferPair.ns) return false;
  const bool treasury =
      std::any_of(treasuries.begin(), treasuries.end(), [&](const TreasuryConfig& t) {
        return t.members.contains(a.hdr.venue) && t.members.contains(b.hdr.venue) &&
               same_currency(a.asset.view(), t.asset);
      });
  return treasury || std::any_of(scopes.begin(), scopes.end(), [&](const TransferScope& s) {
           return s.members.contains(a.hdr.venue) && s.members.contains(b.hdr.venue) &&
                  std::any_of(s.assets.begin(), s.assets.end(), [&](const FixedString<8>& x) {
                    return same_currency(x.view(), a.asset.view());
                  });
         });
}

}  // namespace fastmm::sim
