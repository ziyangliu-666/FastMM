#pragma once
// SimTreasuryPort: a pool treasury (core/treasury.hpp) over simulated accounts, so the same
// Treasury that fastmm-live drives runs against a SimTransport (a backtest's pool of
// [backtest.venues.<member>.balances] accounts, a test). A transfer is carried out `latency` after
// it is submitted, at that venue time (SimTransport::transfer: Done, or Failed when the sending
// account lacks the free amount then); until then status() answers Pending. With no latency it is
// carried out when submitted. Its state is kept by client id, and the accounts' new balances reach
// the engine as BalanceMsg on each account's link. Everything runs on simulated time:
// deterministic.
//
// Control path: a std::map per transfer.
#include "fastmm/core/time.hpp"
#include "fastmm/core/transfer.hpp"
#include "fastmm/core/treasury.hpp"
#include "fastmm/sim/sim_transport.hpp"

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fastmm::sim {

// A transfer a simulated pool carried out or refused.
struct SimTransferRecord {
  Timestamp ts;  // venue time it was carried out (or refused)
  VenueId from;
  VenueId to;
  std::string asset;
  Notional amount;
  TransferState state = TransferState::Done;
  std::string client_id;
};

class SimTreasuryPort final : public TreasuryPort {
 public:
  using Clock = std::function<Timestamp()>;

  SimTreasuryPort(SimTransport& sim, Clock now, Duration latency = Duration{})
      : sim_(&sim), now_(std::move(now)), latency_(latency) {}

  TransferResult submit(const TransferRequest& req) override {
    const Timestamp now = now_();
    if (latency_.ns <= 0) return carry_out(req, now);
    pending_.push_back(Pending{req, now + latency_});
    return {TransferState::Pending, {}, {}};
  }
  TransferResult status(const TransferRequest& req) override {
    settle(now_());
    if (const auto it = done_.find(req.client_id); it != done_.end()) return it->second;
    for (const Pending& p : pending_) {
      if (p.req.client_id == req.client_id) return {TransferState::Pending, {}, {}};
    }
    return {TransferState::NotFound, {}, {}};
  }

  // Carries out the transfers due at or before `now`, in the order they were submitted.
  void settle(Timestamp now) {
    while (!pending_.empty() && pending_.front().due <= now) {
      const Pending p = pending_.front();
      pending_.pop_front();
      static_cast<void>(carry_out(p.req, p.due));
    }
  }
  // When the next submitted transfer is carried out; Timestamp::max() without one.
  [[nodiscard]] Timestamp next_due() const noexcept {
    return pending_.empty() ? Timestamp::max() : pending_.front().due;
  }

  [[nodiscard]] std::size_t transfers() const noexcept { return done_.size(); }
  [[nodiscard]] const std::vector<SimTransferRecord>& records() const noexcept { return records_; }

 private:
  struct Pending {
    TransferRequest req;
    Timestamp due;
  };

  TransferResult carry_out(const TransferRequest& req, Timestamp at) {
    TransferResult r;
    r.state = sim_->transfer(req.from, req.to, req.asset, req.amount, at);
    r.venue_ref = std::to_string(++next_ref_);
    if (r.state == TransferState::Failed) r.detail = "the sending account lacks the free amount";
    done_[req.client_id] = r;
    records_.push_back(
        SimTransferRecord{at, req.from, req.to, req.asset, req.amount, r.state, req.client_id});
    return r;
  }

  SimTransport* sim_;
  Clock now_;
  Duration latency_;
  std::deque<Pending> pending_;
  std::map<std::string, TransferResult> done_;
  std::vector<SimTransferRecord> records_;
  std::uint64_t next_ref_ = 0;
};

}  // namespace fastmm::sim
