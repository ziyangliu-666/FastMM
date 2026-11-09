#pragma once
// The pool treasuries of fastmm-live ([venues.<primary>.treasury], core/treasury.hpp) on a thread
// of their own, fm-treasury: the transfers are blocking REST requests (Venue::transfer) that must
// not hold the engine, a network thread or the control thread.
//
//   LiveTreasuryPort  one pool's TreasuryPort: the primary's connector carries the transfers out
//                     (it holds the transfer key), each account's connector names its account
//                     (Venue::transfer_account) and refreshes its balances on its reactor
//                     (Venue::request_balances); every step goes to the engine as a
//                     ControlTransferMsg on the treasury ring, which the journal records.
//   TreasuryThread    looks at every treasury once a second with the engine's balance table
//                     (EngineLiveStats::balances, the engine's estimate: free less what its own
//                     orders hold) and its view of each pool's funding
//                     (IEngineRunner::pool_funding: the needs by home, the payout it holds), and
//                     publishes their counters for the status file. It also runs the pools' home
//                     planners (core/pool_homes.hpp): each reads the engine's orders by instrument
//                     (Engine::pool_load) and its moves reach the engine as
//                     ControlCommand::PoolHome on the same ring, which the journal records.
//                     And it runs the pools' transfer worker (core/transfer_worker.hpp): every
//                     50 ms it takes the engine's lane intents (IEngineRunner::pop_transfer_intent)
//                     and carries them out; its answers go to the engine on the same ring.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/pool_homes.hpp"
#include "fastmm/core/result.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/status_segment.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/transfer_worker.hpp"
#include "fastmm/core/treasury.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fastmm::venues {
class Venue;
}

namespace fastmm::live {

class LiveTreasuryPort final : public TreasuryPort {
 public:
  using VenueOf = std::function<venues::Venue*(VenueId)>;
  using Refresh = std::function<void(VenueId)>;
  using Wake = std::function<void()>;

  // `journal`: the ring the engine reads the treasury's records from (this port's thread is its
  // only producer); `wake` tells an engine blocked while idle that it has a record.
  LiveTreasuryPort(VenueId primary, VenueOf venue, Refresh refresh, MsgRing* journal, Wake wake);

  TransferResult submit(const TransferRequest& req) override;
  TransferResult status(const TransferRequest& req) override;
  void refresh(VenueId account) override;
  void record(const TreasuryEvent& e) override;

  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }

 private:
  // The request with the accounts' names the venue knows them by, or an error.
  [[nodiscard]] bool named(const TransferRequest& req,
                           TransferRequest& out,
                           std::string& why) const;

  VenueId primary_;
  VenueOf venue_;
  Refresh refresh_;
  MsgRing* journal_;
  Wake wake_;
  std::uint64_t dropped_ = 0;  // records the ring had no room for
};

class TreasuryThread {
 public:
  // Fills `out` with the engine's balance rows (called on the treasury thread).
  using Balances = std::function<void(std::vector<TreasuryBalance>& out)>;
  // Wall-clock ns; tests drive it.
  using Clock = std::function<std::int64_t()>;

  struct Pool {
    std::unique_ptr<Treasury> treasury;
    std::unique_ptr<TreasuryPort> port;
  };

  TreasuryThread(std::vector<Pool> pools, Balances balances, Clock clock = {});

  // The engine's view of a pool's funding (IEngineRunner::pool_funding), called on the thread
  // before each pool's look; false: none. Before start().
  using Funding = std::function<bool(VenueId primary, std::string_view asset, PoolFunding& out)>;
  void set_funding(Funding funding) { funding_ = std::move(funding); }

  // The engine's view of a pool (IEngineRunner::pool_load), and where a planner's moves go (the
  // treasury ring). Both called on the thread.
  using Load = std::function<bool(VenueId primary, PoolLoad& out)>;
  using Emit = std::function<bool(const ControlMsg& m)>;
  // Before start(): the planners this thread runs on wall-clock time.
  void set_planners(std::vector<std::unique_ptr<HomePlanner>> planners, Load load, Emit emit);
  [[nodiscard]] std::size_t planners() const noexcept { return planners_.size(); }
  // Before start(): the pools' transfer worker, the ports it carries transfers out through (owned
  // here), and where it takes the engine's intents from (called on the thread).
  void set_lanes(std::unique_ptr<TransferWorker> worker,
                 std::vector<std::unique_ptr<TreasuryPort>> ports,
                 TransferWorker::Pull pull);
  [[nodiscard]] const TransferWorker* lanes() const noexcept { return worker_.get(); }
  // Homes the planners have moved or given (any thread).
  [[nodiscard]] std::uint64_t home_moves() const noexcept {
    return home_moves_.load(std::memory_order_relaxed);
  }
  ~TreasuryThread();
  TreasuryThread(const TreasuryThread&) = delete;
  TreasuryThread& operator=(const TreasuryThread&) = delete;

  void start();
  // Reads every treasury's ledger again, before start(): a warm standby that took over, whose
  // predecessor wrote the ledgers last.
  [[nodiscard]] Result<void, std::string> reload();
  // Asks the thread to stop after the request it is in; join() waits for it.
  void request_stop() noexcept;
  void join();

  // One pass over every treasury on the calling thread (the thread's loop body; tests).
  void step();

  // The counters summed over the pools, for the status file. Any thread.
  [[nodiscard]] StatusTreasury stats() const noexcept;
  [[nodiscard]] std::size_t pools() const noexcept { return pools_.size(); }

 private:
  void run();
  void publish();

  std::vector<Pool> pools_;
  std::vector<std::unique_ptr<HomePlanner>> planners_;
  std::unique_ptr<TransferWorker> worker_;
  std::vector<std::unique_ptr<TreasuryPort>> lane_ports_;
  TransferWorker::Pull pull_;
  std::int64_t next_slow_ns_ = 0;  // the treasuries' and planners' next pass (once a second)
  Load load_;
  Emit emit_;
  std::unique_ptr<PoolLoad> pool_load_;
  std::atomic<std::uint64_t> home_moves_{0};
  Balances balances_;
  Funding funding_;
  PoolFunding pool_funding_;
  Clock clock_;
  std::vector<TreasuryBalance> rows_;
  Seqlocked<StatusTreasury> stats_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace fastmm::live
