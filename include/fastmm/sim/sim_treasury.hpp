#pragma once
// SimTreasuryPort: a pool treasury (core/treasury.hpp) over simulated accounts, so the same
// Treasury that fastmm-live drives runs against a SimTransport (a backtest's pool of
// [backtest.venues.<member>.balances] accounts, a test). A transfer is carried out when it is
// submitted, at the venue time `now` gives (SimTransport::transfer: Done, or Failed when the
// sending account lacks the free amount); its state is kept by client id for status(), and the
// accounts' new balances reach the engine as BalanceMsg on each account's link.
//
// Control path: a std::map per transfer.
#include "fastmm/core/time.hpp"
#include "fastmm/core/transfer.hpp"
#include "fastmm/core/treasury.hpp"
#include "fastmm/sim/sim_transport.hpp"

#include <functional>
#include <map>
#include <string>
#include <utility>

namespace fastmm::sim {

class SimTreasuryPort final : public TreasuryPort {
 public:
  using Clock = std::function<Timestamp()>;

  SimTreasuryPort(SimTransport& sim, Clock now) : sim_(&sim), now_(std::move(now)) {}

  TransferResult submit(const TransferRequest& req) override {
    TransferResult r;
    r.state = sim_->transfer(req.from, req.to, req.asset, req.amount, now_());
    r.venue_ref = std::to_string(++next_ref_);
    if (r.state == TransferState::Failed) r.detail = "the sending account lacks the free amount";
    done_[req.client_id] = r;
    return r;
  }
  TransferResult status(const TransferRequest& req) override {
    const auto it = done_.find(req.client_id);
    return it == done_.end() ? TransferResult{TransferState::NotFound, {}, {}} : it->second;
  }

  [[nodiscard]] std::size_t transfers() const noexcept { return done_.size(); }

 private:
  SimTransport* sim_;
  Clock now_;
  std::map<std::string, TransferResult> done_;
  std::uint64_t next_ref_ = 0;
};

}  // namespace fastmm::sim
