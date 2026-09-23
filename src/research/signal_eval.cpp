#include "fastmm/research/signal_eval.hpp"

#include "fastmm/core/fixed_point.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace fastmm::research {

namespace {

constexpr double kBps = 10'000.0;
const double kNan = std::numeric_limits<double>::quiet_NaN();

// Ranks of `v`, ties given the average of the positions they occupy.
std::vector<double> ranks_of(const std::vector<double>& v) {
  const std::size_t n = v.size();
  std::vector<std::uint32_t> order(n);
  std::iota(order.begin(), order.end(), 0U);
  std::sort(
      order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return v[a] < v[b]; });
  std::vector<double> r(n, 0.0);
  std::size_t i = 0;
  while (i < n) {
    std::size_t j = i + 1;
    while (j < n && v[order[j]] == v[order[i]]) ++j;
    const double mean = (static_cast<double>(i) + static_cast<double>(j - 1)) / 2.0;
    for (std::size_t k = i; k < j; ++k) r[order[k]] = mean;
    i = j;
  }
  return r;
}

double pearson(const std::vector<double>& a,
               const std::vector<double>& b,
               std::size_t lo,
               std::size_t hi) {
  const std::size_t n = hi - lo;
  if (n < 2) return kNan;
  double sa = 0.0;
  double sb = 0.0;
  for (std::size_t i = lo; i < hi; ++i) {
    sa += a[i];
    sb += b[i];
  }
  const double ma = sa / static_cast<double>(n);
  const double mb = sb / static_cast<double>(n);
  double cov = 0.0;
  double va = 0.0;
  double vb = 0.0;
  for (std::size_t i = lo; i < hi; ++i) {
    const double da = a[i] - ma;
    const double db = b[i] - mb;
    cov += da * db;
    va += da * da;
    vb += db * db;
  }
  if (va <= 0.0 || vb <= 0.0) return kNan;
  return cov / std::sqrt(va * vb);
}

// Spearman over [lo, hi): ranks are recomputed inside the range, so a block's coefficient does
// not borrow the ranking of the rest of the sample.
double spearman(const std::vector<double>& a,
                const std::vector<double>& b,
                std::size_t lo,
                std::size_t hi) {
  if (hi - lo < 2) return kNan;
  const std::vector<double> sa(a.begin() + static_cast<std::ptrdiff_t>(lo),
                               a.begin() + static_cast<std::ptrdiff_t>(hi));
  const std::vector<double> sb(b.begin() + static_cast<std::ptrdiff_t>(lo),
                               b.begin() + static_cast<std::ptrdiff_t>(hi));
  const std::vector<double> ra = ranks_of(sa);
  const std::vector<double> rb = ranks_of(sb);
  return pearson(ra, rb, 0, ra.size());
}

}  // namespace

std::string_view to_string(Feature f) noexcept {
  switch (f) {
    case Feature::Imbalance:
      return "imbalance";
    case Feature::MicropriceEdge:
      return "microprice_edge_bps";
    case Feature::Spread:
      return "spread_bps";
  }
  return "unknown";
}

std::vector<double> feature_values(const FeatureTable& t, Feature f) {
  const FeatureRows& r = t.rows;
  std::vector<double> out(r.size(), 0.0);
  for (std::size_t i = 0; i < r.size(); ++i) {
    const double mid = static_cast<double>(r.mid[i]);
    switch (f) {
      case Feature::Imbalance:
        out[i] = static_cast<double>(r.imbalance[i]) / static_cast<double>(kFixedScale);
        break;
      case Feature::MicropriceEdge:
        out[i] = mid == 0.0 ? kNan : (static_cast<double>(r.microprice[i]) - mid) / mid * kBps;
        break;
      case Feature::Spread:
        out[i] = mid == 0.0 ? kNan : static_cast<double>(r.spread[i]) / mid * kBps;
        break;
    }
  }
  return out;
}

SignalEval evaluate_signal(const FeatureTable& t,
                           std::span<const double> values,
                           std::string_view name,
                           const EvalConfig& cfg) {
  const FeatureRows& r = t.rows;
  if (values.size() != r.size())
    throw std::invalid_argument("evaluate_signal: values must have one entry per row");
  if (cfg.buckets == 0) throw std::invalid_argument("evaluate_signal: buckets must be > 0");
  if (cfg.blocks == 0) throw std::invalid_argument("evaluate_signal: blocks must be > 0");

  SignalEval out;
  out.signal = std::string(name);
  out.rows = r.size();
  out.blocks = cfg.blocks;
  out.horizons.resize(r.forward_mid.size());

  for (std::size_t j = 0; j < r.forward_mid.size(); ++j) {
    HorizonEval& h = out.horizons[j];
    h.horizon_ns = j < r.horizon_ns.size() ? r.horizon_ns[j] : 0;
    if (j < t.coverage.size()) {
      h.excluded_past_end = t.coverage[j].excluded_past_end;
      h.excluded_no_mid = t.coverage[j].excluded_no_mid;
    }
    const std::vector<std::int64_t>& fwd = r.forward_mid[j];
    // Rows measured at this horizon, in row order: a forward mid and a usable signal value.
    std::vector<std::uint32_t> keep;
    keep.reserve(fwd.size());
    for (std::size_t i = 0; i < fwd.size(); ++i) {
      if (fwd[i] != 0 && r.mid[i] != 0 && std::isfinite(values[i]))
        keep.push_back(static_cast<std::uint32_t>(i));
    }
    h.n = keep.size();
    h.ic = kNan;
    if (keep.empty()) continue;

    std::vector<double> sig(keep.size());
    std::vector<double> move(keep.size());  // (forward mid - mid) / mid, basis points
    for (std::size_t k = 0; k < keep.size(); ++k) {
      const std::uint32_t i = keep[k];
      const double mid = static_cast<double>(r.mid[i]);
      sig[k] = values[i];
      move[k] = (static_cast<double>(fwd[i]) - mid) / mid * kBps;
    }
    h.ic = spearman(sig, move, 0, sig.size());

    const auto nblocks = static_cast<std::size_t>(cfg.blocks);
    h.block_ic.reserve(nblocks);
    for (std::size_t b = 0; b < nblocks; ++b) {
      const std::size_t lo = keep.size() * b / nblocks;
      const std::size_t hi = keep.size() * (b + 1) / nblocks;
      h.block_ic.push_back(spearman(sig, move, lo, hi));
    }
    double sum = 0.0;
    std::size_t good = 0;
    std::size_t agree = 0;
    h.block_ic_min = kNan;
    h.block_ic_max = kNan;
    for (double v : h.block_ic) {
      if (!std::isfinite(v)) continue;
      sum += v;
      if (good == 0 || v < h.block_ic_min) h.block_ic_min = v;
      if (good == 0 || v > h.block_ic_max) h.block_ic_max = v;
      ++good;
      if (std::isfinite(h.ic) && ((v > 0.0) == (h.ic > 0.0))) ++agree;
    }
    h.block_ic_mean = good == 0 ? kNan : sum / static_cast<double>(good);
    double var = 0.0;
    for (double v : h.block_ic) {
      if (!std::isfinite(v)) continue;
      const double d = v - h.block_ic_mean;
      var += d * d;
    }
    h.block_ic_stdev = good == 0 ? kNan : std::sqrt(var / static_cast<double>(good));
    h.block_sign_agreement =
        good == 0 ? kNan : static_cast<double>(agree) / static_cast<double>(good);

    // Equal-count buckets of the signal. `order` indexes into keep/sig/move.
    std::vector<std::uint32_t> order(keep.size());
    std::iota(order.begin(), order.end(), 0U);
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
      return sig[a] < sig[b];
    });
    const auto nbuckets = static_cast<std::size_t>(cfg.buckets);
    h.buckets.reserve(nbuckets);
    for (std::size_t b = 0; b < nbuckets; ++b) {
      const std::size_t lo = order.size() * b / nbuckets;
      const std::size_t hi = order.size() * (b + 1) / nbuckets;
      if (lo >= hi) continue;
      SignalBucket bk;
      bk.n = hi - lo;
      bk.lo = sig[order[lo]];
      bk.hi = sig[order[hi - 1]];
      double s_sig = 0.0;
      double s_move = 0.0;
      double s_half = 0.0;
      for (std::size_t k = lo; k < hi; ++k) {
        const std::uint32_t p = order[k];
        const std::uint32_t i = keep[p];
        const double mid = static_cast<double>(r.mid[i]);
        s_sig += sig[p];
        s_move += move[p];
        s_half += (mid - static_cast<double>(r.best_bid[i])) / mid * kBps;
      }
      const auto count = static_cast<double>(bk.n);
      bk.mean_signal = s_sig / count;
      bk.forward_bps = s_move / count;
      bk.half_spread_bps = s_half / count;
      // A resting bid that filled at the touch earns the half spread and then takes the move;
      // a resting ask earns the half spread and pays it.
      bk.buy_bps = bk.half_spread_bps + bk.forward_bps;
      bk.sell_bps = bk.half_spread_bps - bk.forward_bps;
      h.buckets.push_back(bk);
    }
  }
  return out;
}

SignalEval evaluate_feature(const FeatureTable& t, Feature f, const EvalConfig& cfg) {
  const std::vector<double> v = feature_values(t, f);
  return evaluate_signal(t, std::span<const double>(v), to_string(f), cfg);
}

std::string SignalEval::table() const {
  std::string s;
  fmt::format_to(std::back_inserter(s), "signal: {}  rows: {}\n", signal, rows);
  for (const HorizonEval& h : horizons) {
    fmt::format_to(std::back_inserter(s),
                   "\nhorizon {}  measured {}  past end {}  no mid {}\n",
                   h.label(),
                   h.n,
                   h.excluded_past_end,
                   h.excluded_no_mid);
    fmt::format_to(std::back_inserter(s),
                   "  IC (Spearman) {:+.4f}   blocks {}: mean {:+.4f}  sd {:.4f}  "
                   "min {:+.4f}  max {:+.4f}  same sign {:.0f}%\n",
                   h.ic,
                   blocks,
                   h.block_ic_mean,
                   h.block_ic_stdev,
                   h.block_ic_min,
                   h.block_ic_max,
                   h.block_sign_agreement * 100.0);
    if (h.buckets.empty()) continue;
    s += "  bucket      lo        hi       rows   mean sig   fwd bps   half bps    buy bps"
         "   sell bps\n";
    for (std::size_t i = 0; i < h.buckets.size(); ++i) {
      const SignalBucket& b = h.buckets[i];
      fmt::format_to(std::back_inserter(s),
                     "  {:>6}  {:>8.4f}  {:>8.4f}  {:>9}  {:>9.4f}  {:>+8.3f}  {:>8.3f}  "
                     "{:>+9.3f}  {:>+9.3f}\n",
                     i + 1,
                     b.lo,
                     b.hi,
                     b.n,
                     b.mean_signal,
                     b.forward_bps,
                     b.half_spread_bps,
                     b.buy_bps,
                     b.sell_bps);
    }
  }
  return s;
}

}  // namespace fastmm::research
