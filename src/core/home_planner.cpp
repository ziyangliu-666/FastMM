#include "fastmm/core/pool_homes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fastmm {

namespace {
constexpr std::uint16_t kNoSlot = 0xFFFF;
}  // namespace

HomePlanner::HomePlanner(VenueId primary, const PoolPlacement& cfg, std::span<const VenueId> pins)
    : primary_(primary), cfg_(cfg) {
  for (std::size_t i = 0; i < pins.size() && i < pin_.size(); ++i) pin_[i] = pins[i];
  const std::int64_t lookback = std::max<std::int64_t>(cfg.plan_lookback.ns, kWindowNs);
  capacity_ = static_cast<std::size_t>(lookback / kWindowNs);
  slot_.fill(kNoSlot);
}

std::size_t HomePlanner::slot_of(InstrumentId id) const noexcept {
  return id.value < slot_.size() ? slot_[id.value] : kNoSlot;
}

std::vector<HomeMove> HomePlanner::step(std::int64_t now_ns, const PoolLoad& load) {
  if (!started_) {
    started_ = true;
    for (std::size_t j = 0; j < load.count; ++j) {
      const InstrumentId id = load.instrument[j];
      if (id.value >= last_.size()) continue;
      last_[id.value] = load.orders[j];
      seen_[id.value] = true;
    }
    next_sample_ns_ = (now_ns / kWindowNs + 1) * kWindowNs;
    next_plan_ns_ = now_ns + cfg_.plan_interval.ns;
    return {};
  }
  if (now_ns >= next_sample_ns_) {
    sample(load);
    next_sample_ns_ = (now_ns / kWindowNs + 1) * kWindowNs;
  }
  if (!cfg_.planner() || now_ns < next_plan_ns_) return {};
  next_plan_ns_ = now_ns + cfg_.plan_interval.ns;
  return plan(load);
}

void HomePlanner::sample(const PoolLoad& load) {
  if (slots_.empty()) {
    for (std::size_t j = 0; j < load.count && slots_.size() < kNoSlot; ++j) {
      const InstrumentId id = load.instrument[j];
      if (id.value >= slot_.size() || slot_[id.value] != kNoSlot) continue;
      slot_[id.value] = static_cast<std::uint16_t>(slots_.size());
      slots_.push_back(id);
    }
    counts_.assign(slots_.size() * capacity_, 0);
  }
  for (std::size_t j = 0; j < load.count; ++j) {
    const InstrumentId id = load.instrument[j];
    const std::size_t s = slot_of(id);
    if (s == kNoSlot) continue;
    const std::uint64_t now = load.orders[j];
    const std::uint64_t was = seen_[id.value] ? last_[id.value] : now;
    const std::uint64_t d = now >= was ? now - was : 0;
    last_[id.value] = now;
    seen_[id.value] = true;
    counts_[s * capacity_ + head_] =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(d, UINT32_MAX));
  }
  head_ = (head_ + 1) % capacity_;
  filled_ = std::min(filled_ + 1, capacity_);
  ++stats_.samples;
}

std::vector<HomeMove> HomePlanner::plan(const PoolLoad& load) {
  std::vector<HomeMove> out;
  ++stats_.plans;
  const std::size_t n = slots_.size();
  const std::size_t accounts = load.members.size();
  if (filled_ == 0 || n == 0 || accounts < 2) return out;
  const std::size_t windows = filled_;
  // x[s * windows + w]: instrument s's orders in window w, oldest first; tot[s]: their sum.
  std::vector<double> x(n * windows, 0.0);
  std::vector<double> tot(n, 0.0);
  double sum = 0.0;
  for (std::size_t s = 0; s < n; ++s) {
    for (std::size_t w = 0; w < windows; ++w) {
      const double c = counts_[s * capacity_ + (head_ + capacity_ - filled_ + w) % capacity_];
      x[s * windows + w] = c;
      tot[s] += c;
    }
    sum += tot[s];
  }
  if (sum <= 0.0) return out;
  std::array<double, PoolMembers::kMax> lim{};
  double usable_lim = 0.0;
  std::size_t usable = 0;
  for (std::size_t a = 0; a < accounts; ++a) {
    lim[a] = static_cast<double>(load.limit_10s[a] > 0 ? load.limit_10s[a] : kDefaultLimit);
    if (!load.usable[a]) continue;
    usable_lim += lim[a];
    ++usable;
  }
  if (usable == 0) return out;
  // The even level of an account's orders over the lookback in units of its limit: what each
  // would carry if the pool's orders were split by limit.
  const double even = sum / usable_lim;
  // Each instrument's account (index in members; -1: no home), and each account's load by window.
  std::vector<int> at(n, -1);
  std::vector<bool> pinned_now(n, false);
  for (std::size_t j = 0; j < load.count; ++j) {
    const std::size_t s = slot_of(load.instrument[j]);
    if (s == kNoSlot) continue;
    pinned_now[s] = load.pinned[j];
    for (std::size_t a = 0; a < accounts; ++a) {
      if (load.members[a] == load.home[j]) at[s] = static_cast<int>(a);
    }
  }
  std::vector<double> held(accounts * windows, 0.0);  // held[a * windows + w]
  const auto add = [&](std::size_t s, std::size_t a, double sign) {
    for (std::size_t w = 0; w < windows; ++w) held[a * windows + w] += sign * x[s * windows + w];
  };
  std::array<double, PoolMembers::kMax> share{};  // each account's orders over its limit, summed
  for (std::size_t s = 0; s < n; ++s) {
    if (at[s] < 0) continue;
    const auto a = static_cast<std::size_t>(at[s]);
    add(s, a, 1.0);
    share[a] += tot[s] / lim[a];
  }
  const auto phi = [](double orders, double limit) {
    const double over = std::max(0.0, orders / limit - kShare);
    return over * over;
  };
  const double bal_weight = kBalance * static_cast<double>(windows) / static_cast<double>(usable);
  const auto bal = [&](double t) {
    const double r = t / even;
    return bal_weight * r * r;
  };
  // The change in cost when instrument s joins account a (sign 1) or leaves it (sign -1).
  const auto delta = [&](std::size_t s, std::size_t a, double sign) {
    double d = bal(share[a] + sign * tot[s] / lim[a]) - bal(share[a]);
    for (std::size_t w = 0; w < windows; ++w) {
      const double xs = x[s * windows + w];
      if (xs == 0.0) continue;
      const double h = held[a * windows + w];
      d += phi(h + sign * xs, lim[a]) - phi(h, lim[a]);
    }
    return d;
  };
  const auto pinned = [&](std::size_t s) { return pinned_now[s] || pin_[slots_[s].value].valid(); };
  const auto apply = [&](std::size_t s, std::size_t to) {
    const int from = at[s];
    if (from >= 0) {
      add(s, static_cast<std::size_t>(from), -1.0);
      share[static_cast<std::size_t>(from)] -= tot[s] / lim[static_cast<std::size_t>(from)];
    }
    add(s, to, 1.0);
    share[to] += tot[s] / lim[to];
    at[s] = static_cast<int>(to);
    out.push_back(HomeMove{slots_[s],
                           from >= 0 ? load.members[static_cast<std::size_t>(from)] : VenueId{},
                           load.members[to]});
  };
  // First homes: an instrument with orders and no home goes where it costs least.
  for (std::size_t s = 0; s < n; ++s) {
    if (at[s] >= 0 || tot[s] <= 0.0 || pinned(s)) continue;
    std::size_t best = accounts;
    double best_cost = 0.0;
    for (std::size_t a = 0; a < accounts; ++a) {
      if (!load.usable[a]) continue;
      const double c = delta(s, a, 1.0);
      if (best == accounts || c < best_cost) {
        best = a;
        best_cost = c;
      }
    }
    if (best == accounts) continue;
    apply(s, best);
    ++stats_.assigned;
  }
  // Moves: each time the instrument and account that lower the cost most, by at least kMinGain
  // per window.
  const double min_gain = kMinGain * static_cast<double>(windows);
  for (std::uint32_t m = 0; m < cfg_.plan_max_moves; ++m) {
    std::size_t best_s = n;
    std::size_t best_to = accounts;
    double best_gain = min_gain;
    for (std::size_t s = 0; s < n; ++s) {
      if (at[s] < 0 || tot[s] <= 0.0 || pinned(s)) continue;
      const auto from = static_cast<std::size_t>(at[s]);
      const double leave = delta(s, from, -1.0);
      for (std::size_t a = 0; a < accounts; ++a) {
        if (a == from || !load.usable[a]) continue;
        const double gain = -(leave + delta(s, a, 1.0));
        if (gain > best_gain) {
          best_s = s;
          best_to = a;
          best_gain = gain;
        }
      }
    }
    if (best_s == n) break;
    apply(best_s, best_to);
    ++stats_.moves;
  }
  double cost = 0.0;
  for (std::size_t a = 0; a < accounts; ++a) {
    cost += bal(share[a]);
    for (std::size_t w = 0; w < windows; ++w) cost += phi(held[a * windows + w], lim[a]);
  }
  stats_.last_cost = cost / static_cast<double>(windows);
  return out;
}

}  // namespace fastmm
