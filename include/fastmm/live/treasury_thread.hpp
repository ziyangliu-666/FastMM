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
//                     orders hold) and publishes their counters for the status file.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/result.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/status_segment.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/treasury.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
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

// Fills a ControlTransferMsg from a treasury event (hdr.venue: the pool's primary).
void fill_transfer_msg(ControlTransferMsg& m, VenueId primary, const TreasuryEvent& e) noexcept;

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
  Balances balances_;
  Clock clock_;
  std::vector<TreasuryBalance> rows_;
  Seqlocked<StatusTreasury> stats_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace fastmm::live
