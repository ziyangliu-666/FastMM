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
// Faults ([backtest] transfer_p_fail, transfer_p_unknown, transfer_hang_ms), drawn per transfer
// from a generator of the run's seed: with p_fail the venue refuses the transfer (Failed, nothing
// moves); with p_unknown the answer to the request is lost (Unknown) and the venue carries the
// transfer out only `hang` later, answering Pending until then, so a treasury's timeout_s below it
// times the transfer out before it resolves.
//
// Control path: a std::map per transfer.
#include "fastmm/core/rng.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/transfer.hpp"
#include "fastmm/core/treasury.hpp"
#include "fastmm/sim/sim_transport.hpp"

#include <algorithm>
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

struct SimTransferFaults {
  double p_fail = 0.0;     // the venue refuses the transfer
  double p_unknown = 0.0;  // the answer is lost and the transfer hangs for `hang`
  Duration hang{};
  std::uint64_t seed = 1;
};

class SimTreasuryPort final : public TreasuryPort {
 public:
  using Clock = std::function<Timestamp()>;
  // Where the treasury's records go (the engine's feed in a backtest); none: dropped.
  using Record = std::function<void(const TreasuryEvent&)>;

  SimTreasuryPort(SimTransport& sim,
                  Clock now,
                  Duration latency = Duration{},
                  SimTransferFaults faults = {})
      : sim_(&sim), now_(std::move(now)), latency_(latency), faults_(faults), rng_(faults.seed) {}

  void set_record(Record r) { record_ = std::move(r); }

  TransferResult submit(const TransferRequest& req) override {
    const Timestamp now = now_();
    const bool draw = faults_.p_fail > 0.0 || faults_.p_unknown > 0.0;
    const double u = draw ? rng_.uniform01() : 1.0;
    if (u < faults_.p_fail) {
      TransferResult r{TransferState::Failed, {}, "refused by the simulated venue"};
      done_[req.client_id] = r;
      records_.push_back(
          SimTransferRecord{now, req.from, req.to, req.asset, req.amount, r.state, req.client_id});
      ++refused_;
      return r;
    }
    const bool lost = u < faults_.p_fail + faults_.p_unknown;
    const Duration delay{latency_.ns + (lost ? faults_.hang.ns : 0)};
    if (delay.ns <= 0) return carry_out(req, now);
    const Pending p{req, now + delay};
    // In the order they are carried out; equal times keep the order of submission.
    pending_.insert(
        std::upper_bound(pending_.begin(),
                         pending_.end(),
                         p,
                         [](const Pending& a, const Pending& b) { return a.due < b.due; }),
        p);
    if (!lost) return {TransferState::Pending, {}, {}};
    ++lost_;
    return {TransferState::Unknown, {}, "the answer was lost (simulated)"};
  }
  TransferResult status(const TransferRequest& req) override {
    settle(now_());
    if (const auto it = done_.find(req.client_id); it != done_.end()) return it->second;
    for (const Pending& p : pending_) {
      if (p.req.client_id == req.client_id) return {TransferState::Pending, {}, {}};
    }
    return {TransferState::NotFound, {}, {}};
  }
  void record(const TreasuryEvent& e) override {
    if (record_) record_(e);
  }

  // Carries out the transfers due at or before `now`, in the order they are due.
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
  // Transfers the faults refused, and whose answer they lost.
  [[nodiscard]] std::uint64_t refused() const noexcept { return refused_; }
  [[nodiscard]] std::uint64_t lost() const noexcept { return lost_; }

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
  SimTransferFaults faults_;
  Xoshiro256ss rng_;
  Record record_;
  std::deque<Pending> pending_;
  std::map<std::string, TransferResult> done_;
  std::vector<SimTransferRecord> records_;
  std::uint64_t next_ref_ = 0;
  std::uint64_t refused_ = 0;
  std::uint64_t lost_ = 0;
};

}  // namespace fastmm::sim
