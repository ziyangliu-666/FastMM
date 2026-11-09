#pragma once
// The transfer worker: carries out the transfers a pool's lane decides (core/transfer_lane.hpp)
// with the venue's blocking requests, off the engine thread. fastmm-live runs it on fm-treasury
// beside the pool treasuries and home planners (live/treasury_thread.hpp); a backtest runs it on
// simulated time over SimTreasuryPort (sim/sim_treasury.hpp).
//
// From the engine, on a ring (LaneIntent): Send a transfer the engine holds, or Resolved, one the
// engine saw done from both accounts' flows. To the engine, through TreasuryPort::record as
// ControlTransferMsg with kLane: Sent (the venue's answer: Pending, Done or Unknown), Failed, Done
// (from the history), and at the start Reserve for each transfer an earlier process left in flight,
// so the engine holds it again. A transfer the engine has not confirmed status_interval after it
// was sent, or one whose answer was lost, is asked for by its client id every status_interval; one
// the venue has no record of after timeout failed (nothing moved).
//
// The ledger: every transfer not yet resolved, one line each, written atomically (state_file)
// before it is sent and after it resolves. Control path: allocates, blocks.
#include "fastmm/core/account_pool.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/fixed_string.hpp"
#include "fastmm/core/result.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/transfer.hpp"
#include "fastmm/core/transfer_lane.hpp"
#include "fastmm/core/treasury.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm {

// The client id of a lane's transfer (lane_client_id into a FixedString, core/transfer_lane.hpp).
[[nodiscard]] std::string lane_client_id(VenueId primary, std::uint64_t seq);

struct LanePool {
  VenueId primary;
  LaneConfig cfg;
  PoolMembers members;               // the primary first
  std::array<std::string, 8> names;  // members[i]'s name, for the log and the ledger
  TreasuryPort* port = nullptr;      // carries the pool's transfers out; not owned
};

struct TransferWorkerStats {
  std::uint64_t sent = 0;
  std::uint64_t done = 0;
  std::uint64_t failed = 0;
  std::uint64_t unknown = 0;
  std::uint64_t queries = 0;
  std::uint64_t errors = 0;  // ledger writes that failed
  std::uint32_t in_flight = 0;
};

class TransferWorker {
 public:
  using Pull = std::function<bool(LaneIntent&)>;

  explicit TransferWorker(std::vector<LanePool> pools) : pools_(std::move(pools)) {
    state_.resize(pools_.size());
  }

  // Reads each pool's ledger: its transfers are held again and asked for at the first step.
  [[nodiscard]] Result<void, std::string> open();
  // One pass at wall (or simulated) time `now_ns`: the restart's holds, the engine's intents, then
  // the transfers due a status query.
  void step(std::int64_t now_ns, const Pull& pull);
  // When a transfer is next due a query; INT64_MAX without one.
  [[nodiscard]] std::int64_t next_due_ns() const noexcept;

  [[nodiscard]] const TransferWorkerStats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::size_t pools() const noexcept { return pools_.size(); }
  [[nodiscard]] const LanePool& pool(std::size_t i) const noexcept { return pools_[i]; }

  struct Entry {
    TransferRequest req;
    std::uint64_t seq = 0;
    std::int64_t next_poll_ns = 0;
    bool answered = false;  // the venue answered the request (Pending)
  };
  // The ledger's text for pool `i` (tests), and its parse.
  [[nodiscard]] std::string serialize(std::size_t i) const;
  [[nodiscard]] static Result<std::vector<Entry>, std::string> parse(const LanePool& p,
                                                                     std::string_view text);
  [[nodiscard]] const std::vector<Entry>& in_flight(std::size_t i) const noexcept {
    return state_[i].entries;
  }

 private:
  struct PoolState {
    std::vector<Entry> entries;
    bool reheld = false;
  };
  [[nodiscard]] std::size_t pool_of(VenueId primary) const noexcept;
  void send(std::size_t p, const LaneIntent& in, std::int64_t now_ns);
  void poll(std::size_t p, std::size_t k, std::int64_t now_ns);
  void drop(std::size_t p, std::size_t k);
  void record(std::size_t p, TreasuryEventKind kind, const Entry& e, const TransferResult* r) const;
  void persist(std::size_t p);
  void count();

  std::vector<LanePool> pools_;
  std::vector<PoolState> state_;
  TransferWorkerStats stats_;
};

}  // namespace fastmm
