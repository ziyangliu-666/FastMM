#include "fastmm/store/ab_readout.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace fastmm::store {

namespace {

// One arm's sums over one UTC day.
struct DaySums {
  double hours = 0.0;
  double fills = 0.0;
  double notional = 0.0;
  double fees = 0.0;
  std::vector<double> pnl;      // per horizon
  std::vector<double> marked;   // per horizon: the marked fills' notional
  std::vector<double> markout;  // per horizon: side x (mid - price) x qty x multiplier
};

std::uint64_t mix(std::uint64_t x) noexcept {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

std::string horizon_text(std::int32_t ms) {
  if (ms % 60'000 == 0) return fmt::format("{}m", ms / 60'000);
  if (ms % 1'000 == 0) return fmt::format("{}s", ms / 1'000);
  return fmt::format("{}ms", ms);
}

std::string day_text(std::int64_t day) {
  // days since 1970-01-01 -> YYYY-MM-DD (Howard Hinnant's civil_from_days).
  std::int64_t z = day + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2 ? 1 : 0);
  return fmt::format("{:04}-{:02}-{:02}", y, m, d);
}

constexpr std::int64_t kNsPerDay = 86'400'000'000'000LL;

std::int64_t day_of(std::int64_t ns) {
  const std::int64_t q = ns / kNsPerDay;
  return ns % kNsPerDay < 0 ? q - 1 : q;
}

}  // namespace

AbReadout ab_readout(const AbData& data, const AbReadoutOptions& opts) {
  AbReadout r;
  r.test = data.test;
  r.maker_rebate_bps = opts.maker_rebate_bps;
  // The horizons asked for, as indexes into the recorded ones.
  std::vector<std::size_t> hz;
  if (opts.horizons_ms.empty()) {
    for (std::size_t k = 0; k < data.horizons_ms.size(); ++k) hz.push_back(k);
  } else {
    for (const std::int32_t h : opts.horizons_ms) {
      const auto it = std::find(data.horizons_ms.begin(), data.horizons_ms.end(), h);
      if (it == data.horizons_ms.end()) {
        std::string recorded;
        for (const std::int32_t x : data.horizons_ms) recorded += " " + std::to_string(x);
        throw std::invalid_argument(fmt::format("the store has no markouts at {} ms; recorded:{}",
                                                h,
                                                recorded.empty() ? " none" : recorded));
      }
      hz.push_back(static_cast<std::size_t>(it - data.horizons_ms.begin()));
    }
  }
  for (const std::size_t k : hz) r.horizons_ms.push_back(data.horizons_ms[k]);
  const std::size_t nh = hz.size();
  const std::size_t na = data.arms.size();

  // Sums per UTC day, arm and instrument.
  using Cell = std::map<std::string, DaySums>;  // by symbol
  std::map<std::int64_t, std::vector<Cell>> days;
  const auto sums = [&](std::int64_t day, int arm, const std::string& symbol) -> DaySums& {
    auto& v = days[day];
    if (v.empty()) v.resize(na);
    DaySums& s = v[static_cast<std::size_t>(arm)][symbol];
    if (s.pnl.empty()) {
      s.pnl.assign(nh, 0.0);
      s.marked.assign(nh, 0.0);
      s.markout.assign(nh, 0.0);
    }
    return s;
  };
  r.arms.resize(na);
  for (std::size_t a = 0; a < na; ++a) {
    r.arms[a].arm = data.arms[a];
    r.arms[a].params = a < data.params.size() ? data.params[a] : std::string();
    r.arms[a].marked.assign(nh, 0);
    r.arms[a].markout_bps.assign(nh, 0.0);
    r.arms[a].pnl.assign(nh, 0.0);
  }
  for (const AbExposure& e : data.exposure) {
    if (e.arm < 0 || static_cast<std::size_t>(e.arm) >= na) continue;
    sums(e.day, e.arm, e.symbol).hours += e.hours;
  }
  const double rebate = opts.maker_rebate_bps * 1e-4;
  std::vector<std::vector<double>> marked_notional(na, std::vector<double>(nh, 0.0));
  std::vector<std::vector<double>> markout(na, std::vector<double>(nh, 0.0));
  for (const AbFill& f : data.fills) {
    if (f.arm < 0 || static_cast<std::size_t>(f.arm) >= na) continue;
    const auto a = static_cast<std::size_t>(f.arm);
    const double notional = std::abs(f.price * f.qty * f.multiplier);
    const double fee = f.fee - (f.maker && !f.inverse ? rebate * notional : 0.0);
    DaySums& s = sums(day_of(f.ts_ns), f.arm, f.symbol);
    AbArmTotals& t = r.arms[a];
    ++t.fills;
    if (f.maker) ++t.maker_fills;
    t.fees += fee;
    s.fills += 1.0;
    s.fees += fee;
    if (f.inverse) continue;
    t.notional += notional;
    s.notional += notional;
    for (std::size_t j = 0; j < nh; ++j) {
      const double mid = hz[j] < f.mid.size() ? f.mid[hz[j]] : 0.0;
      if (mid <= 0.0) continue;
      const double mo = f.side * (mid - f.price) * f.qty * f.multiplier;
      ++t.marked[j];
      t.pnl[j] += mo - fee;
      marked_notional[a][j] += notional;
      markout[a][j] += mo;
      s.pnl[j] += mo - fee;
      s.marked[j] += notional;
      s.markout[j] += mo;
    }
  }
  for (std::size_t a = 0; a < na; ++a) {
    for (std::size_t j = 0; j < nh; ++j) {
      r.arms[a].markout_bps[j] =
          marked_notional[a][j] > 0.0 ? markout[a][j] / marked_notional[a][j] * 1e4 : 0.0;
    }
  }
  const auto hours = [](const Cell& c) {
    double h = 0.0;
    for (const auto& [sym, s] : c) h += s.hours;
    return h;
  };
  for (const auto& [day, v] : days) {
    for (std::size_t a = 0; a < na; ++a) {
      const double h = hours(v[a]);
      r.arms[a].inst_hours += h;
      if (h > 0.0) ++r.arms[a].days;
    }
  }

  struct Metric {
    std::string name;
    double DaySums::*scalar = nullptr;  // numerator, or null for a per-horizon one
    std::vector<double> DaySums::*vec = nullptr;
    std::size_t j = 0;
    bool bps = false;  // over the marked notional instead of the hours
  };
  std::vector<Metric> metrics;
  for (std::size_t j = 0; j < nh; ++j) {
    metrics.push_back(
        {"markout pnl/hour " + horizon_text(r.horizons_ms[j]), nullptr, &DaySums::pnl, j, false});
  }
  for (std::size_t j = 0; j < nh; ++j) {
    metrics.push_back(
        {"markout bps " + horizon_text(r.horizons_ms[j]), nullptr, &DaySums::markout, j, true});
  }
  metrics.push_back({"notional/hour", &DaySums::notional, nullptr, 0, false});
  metrics.push_back({"fills/hour", &DaySums::fills, nullptr, 0, false});

  r.resamples = opts.resamples;
  std::size_t common = 0;
  for (const auto& [day, v] : days) {
    bool all = true;
    for (std::size_t a = 0; a < na; ++a) all = all && hours(v[a]) > 0.0;
    if (all) ++common;
  }
  r.days = common;
  for (std::size_t a = 1; a < na; ++a) {
    // The clusters: days with time in the control or the arm compared.
    std::vector<const std::vector<Cell>*> cl;
    for (const auto& [day, v] : days) {
      if (hours(v[0]) > 0.0 || hours(v[a]) > 0.0) cl.push_back(&v);
    }
    // Paired by instrument when some instrument ran in both arms (a switchback): each
    // instrument's rate in the arm against its own in the control, weighted by its hours in
    // both. Otherwise the arms' pooled rates.
    std::set<std::string> in_arm;
    std::set<std::string> in_control;
    for (const auto* v : cl) {
      for (const auto& [sym, s] : (*v)[a])
        if (s.hours > 0.0) in_arm.insert(sym);
      for (const auto& [sym, s] : (*v)[0])
        if (s.hours > 0.0) in_control.insert(sym);
    }
    std::vector<std::string> paired;
    std::set_intersection(in_arm.begin(),
                          in_arm.end(),
                          in_control.begin(),
                          in_control.end(),
                          std::back_inserter(paired));
    if (a == 1) r.paired = !paired.empty();
    for (const Metric& m : metrics) {
      const auto num = [&](const DaySums& s) {
        return m.scalar != nullptr ? s.*m.scalar : (s.*m.vec)[m.j];
      };
      const auto den = [&](const DaySums& s) { return m.bps ? s.marked[m.j] : s.hours; };
      const double scale = m.bps ? 1e4 : 1.0;
      // (arm rate, control rate) over the days `pick`.
      const auto rates = [&](const std::vector<std::size_t>& pick) {
        if (paired.empty()) {
          double n[2] = {0.0, 0.0};
          double d[2] = {0.0, 0.0};
          for (const std::size_t i : pick) {
            for (int k = 0; k < 2; ++k) {
              for (const auto& [sym, s] : (*cl[i])[k == 0 ? a : 0]) {
                n[k] += num(s);
                d[k] += den(s);
              }
            }
          }
          return std::tuple{d[0] > 0.0 ? n[0] / d[0] * scale : 0.0,
                            d[1] > 0.0 ? n[1] / d[1] * scale : 0.0,
                            d[0] > 0.0 && d[1] > 0.0};
        }
        double wa = 0.0;
        double wc = 0.0;
        double w = 0.0;
        for (const std::string& sym : paired) {
          double n[2] = {0.0, 0.0};
          double d[2] = {0.0, 0.0};
          for (const std::size_t i : pick) {
            for (int k = 0; k < 2; ++k) {
              const Cell& c = (*cl[i])[k == 0 ? a : 0];
              if (const auto it = c.find(sym); it != c.end()) {
                n[k] += num(it->second);
                d[k] += den(it->second);
              }
            }
          }
          if (!(d[0] > 0.0) || !(d[1] > 0.0)) continue;
          const double weight = d[0] + d[1];
          wa += weight * n[0] / d[0];
          wc += weight * n[1] / d[1];
          w += weight;
        }
        return std::tuple{w > 0.0 ? wa / w * scale : 0.0, w > 0.0 ? wc / w * scale : 0.0, w > 0.0};
      };
      std::vector<std::size_t> all(cl.size());
      for (std::size_t i = 0; i < cl.size(); ++i) all[i] = i;
      AbComparison c;
      c.arm = data.arms[a];
      c.metric = m.name;
      std::tie(c.value, c.control, std::ignore) = rates(all);
      c.diff = c.value - c.control;
      c.lo = c.hi = c.diff;
      if (cl.size() >= 2 && opts.resamples > 0) {
        std::vector<double> draws;
        draws.reserve(opts.resamples);
        std::uint64_t state = mix(opts.seed ^ mix(a));
        std::vector<std::size_t> pick(cl.size());
        for (std::size_t b = 0; b < opts.resamples; ++b) {
          for (std::size_t& p : pick) {
            state = mix(state);
            p = state % cl.size();
          }
          // A draw with no day of an arm (or of a pair) has no difference to give.
          const auto [x, y, ok] = rates(pick);
          if (ok) draws.push_back(x - y);
        }
        std::sort(draws.begin(), draws.end());
        if (draws.empty()) draws.push_back(c.diff);
        const auto at = [&](double q) {
          const auto k =
              static_cast<std::size_t>(std::lround(q * static_cast<double>(draws.size() - 1)));
          return draws[std::min(k, draws.size() - 1)];
        };
        c.lo = at(0.025);
        c.hi = at(0.975);
      }
      r.comparisons.push_back(std::move(c));
    }
  }
  return r;
}

std::string format_ab_readout(const AbReadout& r, const AbData& data) {
  std::string out;
  auto it = std::back_inserter(out);
  fmt::format_to(it,
                 "test     {}: {} session(s), {} UTC day(s) with time in every arm\n",
                 r.test,
                 data.sessions,
                 r.days);
  for (const std::string& w : data.warnings) fmt::format_to(it, "warning  {}\n", w);
  if (r.horizons_ms.empty())
    out += "markouts none recorded: set [engine] markout_ms to mark the fills\n";
  if (r.maker_rebate_bps != 0.0)
    fmt::format_to(it, "fees     less a maker rebate of {} bps\n", r.maker_rebate_bps);
  if (data.inverse_fills != 0)
    fmt::format_to(
        it, "inverse  {} fill(s) on inverse contracts, counted, not marked\n", data.inverse_fills);
  fmt::format_to(it,
                 "\n{:<16} {:>10} {:>5} {:>8} {:>7} {:>16} {:>12}  {}\n",
                 "arm",
                 "inst_hours",
                 "days",
                 "fills",
                 "maker%",
                 "notional",
                 "fees",
                 "params");
  for (const AbArmTotals& a : r.arms) {
    fmt::format_to(it,
                   "{:<16} {:>10.2f} {:>5} {:>8} {:>7.1f} {:>16.2f} {:>12.4f}  {}\n",
                   a.arm,
                   a.inst_hours,
                   a.days,
                   a.fills,
                   a.fills == 0
                       ? 0.0
                       : 100.0 * static_cast<double>(a.maker_fills) / static_cast<double>(a.fills),
                   a.notional,
                   a.fees,
                   a.params.empty() ? std::string("(configured)") : a.params);
  }
  if (!r.horizons_ms.empty()) {
    fmt::format_to(it,
                   "\n{:<16} {:>8} {:>8} {:>12} {:>14} {:>12}\n",
                   "arm",
                   "horizon",
                   "marked",
                   "markout_bps",
                   "markout_pnl",
                   "per hour");
    for (const AbArmTotals& a : r.arms) {
      for (std::size_t j = 0; j < r.horizons_ms.size(); ++j) {
        fmt::format_to(it,
                       "{:<16} {:>8} {:>8} {:>12.3f} {:>14.6f} {:>12.6f}\n",
                       a.arm,
                       horizon_text(r.horizons_ms[j]),
                       a.marked[j],
                       a.markout_bps[j],
                       a.pnl[j],
                       a.inst_hours > 0.0 ? a.pnl[j] / a.inst_hours : 0.0);
      }
    }
  }
  if (!r.comparisons.empty()) {
    fmt::format_to(it,
                   "\nagainst {}, {}: 95% intervals from {} resamples of whole UTC days\n",
                   r.arms.empty() ? std::string() : r.arms.front().arm,
                   r.paired ? "each instrument with itself" : "the arms' instruments pooled",
                   r.resamples);
    fmt::format_to(it,
                   "{:<16} {:<18} {:>14} {:>14} {:>14}  {}\n",
                   "arm",
                   "metric",
                   "arm",
                   "control",
                   "diff",
                   "95% interval");
    for (const AbComparison& c : r.comparisons) {
      fmt::format_to(it,
                     "{:<16} {:<18} {:>14.6f} {:>14.6f} {:>+14.6f}  [{:+.6f}, {:+.6f}]\n",
                     c.arm,
                     c.metric,
                     c.value,
                     c.control,
                     c.diff,
                     c.lo,
                     c.hi);
    }
    if (r.days < 2) out += "(fewer than 2 days with every arm: no interval)\n";
  }
  return out;
}

std::string ab_daily_csv(const AbReadout& r, const AbData& data) {
  std::map<std::pair<std::int64_t, int>, std::vector<double>>
      rows;  // hours fills notional fees pnl...
  const std::size_t nh = r.horizons_ms.size();
  const auto row = [&](std::int64_t day, int arm) -> std::vector<double>& {
    auto& v = rows[{day, arm}];
    if (v.empty()) v.assign(4 + nh, 0.0);
    return v;
  };
  for (const AbExposure& e : data.exposure) row(e.day, e.arm)[0] += e.hours;
  for (const AbFill& f : data.fills) {
    if (f.arm < 0) continue;
    std::vector<double>& v = row(day_of(f.ts_ns), f.arm);
    v[1] += 1.0;
    if (!f.inverse) v[2] += std::abs(f.price * f.qty * f.multiplier);
    v[3] += f.fee;
    for (std::size_t j = 0; j < nh; ++j) {
      const auto k = static_cast<std::size_t>(
          std::find(data.horizons_ms.begin(), data.horizons_ms.end(), r.horizons_ms[j]) -
          data.horizons_ms.begin());
      const double mid = k < f.mid.size() ? f.mid[k] : 0.0;
      if (mid > 0.0 && !f.inverse)
        v[4 + j] += f.side * (mid - f.price) * f.qty * f.multiplier - f.fee;
    }
  }
  std::string out = "day,arm,inst_hours,fills,notional,fees";
  for (const std::int32_t h : r.horizons_ms) out += ",markout_pnl_" + horizon_text(h);
  out += "\n";
  for (const auto& [key, v] : rows) {
    const auto arm = static_cast<std::size_t>(key.second);
    out += fmt::format("{},{},{:.6f},{:.0f},{:.8f},{:.8f}",
                       day_text(key.first),
                       arm < data.arms.size() ? data.arms[arm] : std::to_string(arm),
                       v[0],
                       v[1],
                       v[2],
                       v[3]);
    for (std::size_t j = 0; j < nh; ++j) out += fmt::format(",{:.8f}", v[4 + j]);
    out += "\n";
  }
  return out;
}

}  // namespace fastmm::store
