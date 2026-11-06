#include "fastmm/core/pool_homes.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fastmm {

namespace {
constexpr std::uint16_t kNoSlot = 0xFFFF;
constexpr double kHotQuantile = 0.99;
constexpr double kMinGain = 1e-9;

// The value at quantile q of `v` (sorted in place), interpolated between ranks as numpy's default.
double quantile(std::vector<double>& v, double q) {
  std::sort(v.begin(), v.end());
  const double pos = q * static_cast<double>(v.size() - 1);
  const auto lo = static_cast<std::size_t>(std::floor(pos));
  const std::size_t hi = std::min(lo + 1, v.size() - 1);
  const double frac = pos - static_cast<double>(lo);
  return v[lo] + frac * (v[hi] - v[lo]);
}
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
  const auto window = [&](std::size_t w) { return (head_ + capacity_ - filled_ + w) % capacity_; };
  // The pool's busiest windows: those at or above the 99th percentile of the pool's totals.
  std::vector<double> totals(filled_, 0.0);
  for (std::size_t w = 0; w < filled_; ++w) {
    const std::size_t at = window(w);
    for (std::size_t s = 0; s < n; ++s) totals[w] += counts_[s * capacity_ + at];
  }
  std::vector<double> sorted = totals;
  const double q = quantile(sorted, kHotQuantile);
  std::vector<std::size_t> hot;
  for (std::size_t w = 0; w < filled_; ++w) {
    if (totals[w] >= q && totals[w] > 0.0) hot.push_back(window(w));
  }
  if (hot.empty()) return out;
  const std::size_t h = hot.size();
  std::vector<double> mx(n * h, 0.0);  // mx[s * h + k]: instrument s in hot window k
  std::vector<double> tot(n, 0.0);
  for (std::size_t s = 0; s < n; ++s) {
    for (std::size_t k = 0; k < h; ++k) mx[s * h + k] = counts_[s * capacity_ + hot[k]];
    for (std::size_t w = 0; w < filled_; ++w) tot[s] += counts_[s * capacity_ + window(w)];
  }
  // Each instrument's account (index in members; -1: no home) and each account's limit.
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
  std::array<double, PoolMembers::kMax> lim{};
  std::size_t usable = 0;
  for (std::size_t a = 0; a < accounts; ++a) {
    lim[a] = static_cast<double>(load.limit_10s[a] > 0 ? load.limit_10s[a] : kDefaultLimit);
    usable += load.usable[a] ? 1U : 0U;
  }
  if (usable == 0) return out;
  double sum = 0.0;
  for (const double t : tot) sum += t;
  if (sum <= 0.0) return out;
  const double even = sum / static_cast<double>(usable);
  std::vector<double> acc(accounts * h, 0.0);  // acc[a * h + k]
  std::array<double, PoolMembers::kMax> day{};
  std::array<double, PoolMembers::kMax> peak{};
  const auto add = [&](std::size_t s, std::size_t a, double sign) {
    for (std::size_t k = 0; k < h; ++k) acc[a * h + k] += sign * mx[s * h + k];
    day[a] += sign * tot[s];
  };
  const auto peak_of = [&](std::size_t a) {
    double p = 0.0;
    for (std::size_t k = 0; k < h; ++k) p = std::max(p, acc[a * h + k]);
    return p;
  };
  for (std::size_t s = 0; s < n; ++s) {
    if (at[s] >= 0) add(s, static_cast<std::size_t>(at[s]), 1.0);
  }
  for (std::size_t a = 0; a < accounts; ++a) peak[a] = peak_of(a);
  // The cost with peaks pk and daily loads dy.
  const auto cost_of = [&](const std::array<double, PoolMembers::kMax>& pk,
                           const std::array<double, PoolMembers::kMax>& dy) {
    double p = 0.0;
    double d = 0.0;
    for (std::size_t a = 0; a < accounts; ++a) {
      p = std::max(p, pk[a] / lim[a]);
      d = std::max(d, dy[a] / even);
    }
    return p + d;
  };
  // The cost once instrument s leaves account `from` (-1: none) for account `to`.
  const auto cost_moved = [&](std::size_t s, int from, std::size_t to) {
    std::array<double, PoolMembers::kMax> pk = peak;
    std::array<double, PoolMembers::kMax> dy = day;
    if (from >= 0) {
      const auto f = static_cast<std::size_t>(from);
      double p = 0.0;
      for (std::size_t k = 0; k < h; ++k) p = std::max(p, acc[f * h + k] - mx[s * h + k]);
      pk[f] = p;
      dy[f] -= tot[s];
    }
    double p = 0.0;
    for (std::size_t k = 0; k < h; ++k) p = std::max(p, acc[to * h + k] + mx[s * h + k]);
    pk[to] = p;
    dy[to] += tot[s];
    return cost_of(pk, dy);
  };
  const auto pinned = [&](std::size_t s) { return pinned_now[s] || pin_[slots_[s].value].valid(); };
  const auto apply = [&](std::size_t s, std::size_t to) {
    const int from = at[s];
    if (from >= 0) {
      add(s, static_cast<std::size_t>(from), -1.0);
      peak[static_cast<std::size_t>(from)] = peak_of(static_cast<std::size_t>(from));
    }
    add(s, to, 1.0);
    peak[to] = peak_of(to);
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
      const double c = cost_moved(s, -1, a);
      if (best == accounts || c < best_cost) {
        best = a;
        best_cost = c;
      }
    }
    if (best == accounts) continue;
    apply(s, best);
    ++stats_.assigned;
  }
  // Moves: one instrument off the worst account at a time, where that lowers the cost.
  for (std::uint32_t m = 0; m < cfg_.plan_max_moves; ++m) {
    const double c0 = cost_of(peak, day);
    std::size_t worst = 0;
    double worst_score = -1.0;
    for (std::size_t a = 0; a < accounts; ++a) {
      const double score = peak[a] / lim[a] + day[a] / even;
      if (score > worst_score) {
        worst = a;
        worst_score = score;
      }
    }
    std::size_t best_s = n;
    std::size_t best_to = accounts;
    double best_cost = c0 - kMinGain;
    for (std::size_t s = 0; s < n; ++s) {
      if (at[s] != static_cast<int>(worst) || tot[s] <= 0.0 || pinned(s)) continue;
      for (std::size_t a = 0; a < accounts; ++a) {
        if (a == worst || !load.usable[a]) continue;
        const double c = cost_moved(s, static_cast<int>(worst), a);
        if (c < best_cost) {
          best_s = s;
          best_to = a;
          best_cost = c;
        }
      }
    }
    if (best_s == n) break;
    apply(best_s, best_to);
    ++stats_.moves;
  }
  stats_.last_cost = cost_of(peak, day);
  return out;
}

}  // namespace fastmm
