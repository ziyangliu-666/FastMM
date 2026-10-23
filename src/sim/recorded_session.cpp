#include "fastmm/sim/recorded_session.hpp"

#include <fmt/format.h>

#include <algorithm>

namespace fastmm::sim {

namespace {

void set_asset(SimAccountConfig& a, const FixedString<8>& asset, Notional total) {
  for (SimBalance& b : a.balances) {
    if (b.asset == asset) {
      b.amount = total;
      return;
    }
  }
  a.balances.push_back(SimBalance{asset, total});
}

SimAccountConfig& account_of(std::vector<SimAccountConfig>& out, VenueId v) {
  for (SimAccountConfig& a : out) {
    if (a.venue == v) return a;
  }
  out.push_back(SimAccountConfig{v, {}});
  return out.back();
}

}  // namespace

SessionStart RecordedSession::start(Timestamp at) const {
  SessionStart out;
  const Checkpoint* use = nullptr;
  bool from_restored = false;
  if (at.valid()) {
    for (const Checkpoint& c : checkpoints) {
      if (c.at <= at) use = &c;
    }
    if (use == nullptr) {
      out.note = fmt::format("journal: no checkpoint at or before {} ns", at.ns);
      return out;
    }
  } else if (restored) {
    use = &*restored;
    from_restored = true;
  } else if (!checkpoints.empty()) {
    use = &checkpoints.front();
  }
  if (use != nullptr) {
    out.has_state = true;
    out.strategy = use->strategy;
  }
  if (use != nullptr && !from_restored) {
    out.positions = use->positions;
    out.at = at.valid() ? use->at : Timestamp{};
    out.note = fmt::format("journal: started from the checkpoint of {} ns ({} bytes, {} positions)",
                           use->at.ns,
                           out.strategy.size(),
                           out.positions.size());
    return out;
  }
  for (const ReconcileMsg& m : reconciled) {
    if (!m.position_qty.is_zero()) out.positions.push_back(m);
  }
  if (use != nullptr) {
    out.note = fmt::format(
        "journal: started from the restored state ({} bytes{}) and {} positions reconciled before "
        "the first order",
        out.strategy.size(),
        use->accepted ? ", taken live" : ", refused live",
        out.positions.size());
  } else {
    out.note = fmt::format(
        "journal: no strategy state recorded; {} positions reconciled before the first order",
        out.positions.size());
  }
  return out;
}

std::vector<SimAccountConfig> RecordedSession::balances_at(Timestamp at) const {
  // Each venue's first complete snapshot, and with `at` its account folded up to then.
  std::vector<SimAccountConfig> first;
  std::vector<SimAccountConfig> folded;
  std::vector<VenueId> first_done;
  std::vector<VenueId> in_snapshot;  // venues whose snapshot is being read
  for (const BalanceMsg& m : balances) {
    const VenueId v = m.hdr.venue;
    const bool snap = (m.flags & (BalanceMsg::kSnapshot | BalanceMsg::kSnapshotEnd)) != 0;
    const bool done = std::find(first_done.begin(), first_done.end(), v) != first_done.end();
    if (snap && !done) {
      SimAccountConfig& a = account_of(first, v);
      if (!m.asset.empty()) set_asset(a, m.asset, m.total);
      if ((m.flags & BalanceMsg::kSnapshotEnd) != 0) first_done.push_back(v);
    }
    if (!at.valid() || account_time(m.hdr) > at) continue;
    SimAccountConfig& a = account_of(folded, v);
    const auto open = std::find(in_snapshot.begin(), in_snapshot.end(), v);
    if (snap && open == in_snapshot.end()) {
      a.balances.clear();  // a snapshot names every asset the account holds
      in_snapshot.push_back(v);
    }
    if (!m.asset.empty()) set_asset(a, m.asset, m.total);
    if ((m.flags & BalanceMsg::kSnapshotEnd) != 0) {
      const auto it = std::find(in_snapshot.begin(), in_snapshot.end(), v);
      if (it != in_snapshot.end()) in_snapshot.erase(it);
    }
  }
  if (!at.valid()) return first;
  for (const SimAccountConfig& f : first) {
    const bool reported = std::any_of(folded.begin(), folded.end(), [&](const SimAccountConfig& a) {
      return a.venue == f.venue;
    });
    if (!reported) folded.push_back(f);
  }
  return folded;
}

std::vector<std::pair<VenueId, Timestamp>> RecordedSession::refreshes() const {
  std::vector<std::pair<VenueId, Timestamp>> out;
  for (const BalanceMsg& m : balances) {
    if ((m.flags & BalanceMsg::kSnapshotEnd) != 0)
      out.emplace_back(m.hdr.venue, account_time(m.hdr));
  }
  return out;
}

}  // namespace fastmm::sim
