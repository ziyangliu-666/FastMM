#include "fastmm/backtest/ab_report.hpp"

#include "fastmm/backtest/mid_series.hpp"
#include "fastmm/backtest/own_orders.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/core/fees.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/strategies/ab_test.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace fastmm::bt {

namespace {

constexpr std::int64_t kNoEnd = std::numeric_limits<std::int64_t>::max();
constexpr double kNsPerHour = 3.6e12;

std::int64_t to_raw(double quote) {
  return std::llround(quote * 1e8);
}
double quote(std::int64_t raw) {
  return static_cast<double>(raw) / 1e8;
}

std::string horizon_label(std::int64_t ns) {
  MarkoutHorizon h;
  h.horizon_ns = ns;
  return h.label();
}

// A number for JSON: NaN and infinities have no spelling there.
std::string num(double v) {
  return std::isfinite(v) ? fmt::format("{:.6g}", v) : std::string("null");
}
std::string json_text(std::string_view v) {
  std::string out = "\"";
  for (const char c : v) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

std::uint64_t mix(std::uint64_t x) noexcept {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

std::string arm_params(const AbArmSection& a) {
  std::string out;
  for (const auto& [k, v] : a.params) {
    if (!out.empty()) out += ' ';
    out.append(k).append("=").append(v);
  }
  return out;
}

// One instrument's time in one arm, [start, end).
struct Period {
  std::int64_t start;
  std::int64_t end;
  int arm;
};

struct Opened {
  std::string path;
  std::unique_ptr<JournalReader> reader;
  std::int64_t first_ns = 0;
};

// What the comparison needs of one pair in one arm.
struct PairArm {
  std::int64_t ns = 0;
  std::int64_t notional_raw = 0;
  std::uint64_t fills = 0;
  std::vector<std::int64_t> net_raw;     // per horizon
  std::vector<std::int64_t> marked_raw;  // notional of the fills marked at the horizon
};

class Reading {
 public:
  Reading(const AbReportOptions& opt, AbReport& r) : opt_(opt), r_(r) {}

  void read(Opened& j, bool continues) {
    JournalReader& reader = *j.reader;
    const std::string_view text = reader.config_text();
    if (text.empty()) throw std::runtime_error(j.path + ": no configuration in the journal");
    Config::LoadOptions lo;
    lo.substitute_env = false;
    lo.allow_inline_secrets = true;
    const Config cfg = [&] {
      try {
        return Config::parse(text, lo, j.path + " (embedded)");
      } catch (const std::exception& e) {
        throw std::runtime_error(j.path + ": its configuration does not load: " + e.what());
      }
    }();
    if (!cfg.ab.enabled()) throw std::runtime_error(j.path + ": no [ab] in its configuration");
    check_same_test(cfg.ab, j.path);
    InstrumentTable table;
    for (const Instrument& inst : reader.instruments()) {
      if (!table.add(inst)) throw std::runtime_error(j.path + ": bad instrument table");
    }
    std::vector<std::string> venue_names;
    venue_names.reserve(cfg.venues.size());
    for (const VenueSection& v : cfg.venues) venue_names.push_back(v.name);
    const AbPlan plan = [&] {
      try {
        return AbPlan(cfg.ab, table, venue_names);
      } catch (const std::exception& e) {
        throw std::runtime_error(j.path + ": " + e.what());
      }
    }();
    const FeeTable fees = fee_table(cfg, &table, &venue_names);
    // This journal's instrument ids -> the report's instruments, by "venue:symbol".
    std::vector<std::size_t> global(table.size());
    for (std::size_t i = 0; i < table.size(); ++i) {
      global[i] = instrument(plan.instrument_name(i));
      if (stratum_.size() <= global[i]) stratum_.resize(global[i] + 1, kUnset);
      if (stratum_[global[i]] == kUnset) stratum_[global[i]] = plan.stratum(i);
    }
    if (!continues) {
      close_all(last_ns_);
      order_arm_.clear();
      seen_.clear();
    }
    if (!plan_) plan_.emplace(plan);
    const MidSeries mids(reader);
    r_.mids = r_.mids || !mids.empty();
    const std::size_t horizons = r_.horizons_ns.size();

    reader.for_each([&](const EventHeader* h) {
      const std::int64_t t = venue_ts(*h).ns;
      if (t != 0) {
        if (r_.first_ns == 0 || t < r_.first_ns) r_.first_ns = t;
        last_ns_ = std::max(last_ns_, t);
      }
      const bool out = (h->flags & EventHeader::kOutbound) != 0;
      switch (h->type) {
        case EventType::ParamUpdate: {
          const auto& m = msg_cast<ParamUpdateMsg>(h);
          if (out || m.origin != ParamUpdateMsg::Origin::Experiment ||
              m.arm == ParamUpdateMsg::kArmKeep)
            return;
          const int arm = m.arm == ParamUpdateMsg::kArmNone ? AbPlan::kNoArm : m.arm - 1;
          if (m.all_instruments()) {
            for (const std::size_t g : global) move(g, arm, t);
          } else if (m.hdr.instrument.value < global.size()) {
            move(global[m.hdr.instrument.value], arm, t);
          }
          return;
        }
        case EventType::OutNewOrder:
          if (out)
            order_arm_[msg_cast<OutNewOrderMsg>(h).cl_ord_id.value] =
                msg_cast<OutNewOrderMsg>(h).arm;
          return;
        case EventType::OutReplace:
          if (out)
            order_arm_[msg_cast<OutReplaceMsg>(h).cl_ord_id.value] = msg_cast<OutReplaceMsg>(h).arm;
          return;
        case EventType::OrderFill:
          if (!out) break;
          return;
        default:
          return;
      }
      const auto& f = msg_cast<OrderFillMsg>(h);
      const std::uint32_t inst = h->instrument.value;
      if (inst >= global.size()) return;
      if (!f.exec_id.empty() &&
          !seen_.emplace(inst, f.side, std::string(f.exec_id.view())).second) {
        ++r_.repeated_fills;
        return;
      }
      const auto it = order_arm_.find(f.cl_ord_id.value);
      const int arm = it == order_arm_.end() || it->second == 0 ? AbPlan::kNoArm : it->second - 1;
      const std::optional<std::int64_t> block =
          arm == AbPlan::kNoArm ? std::nullopt : block_of_fill(global[inst], arm, t);
      if (!block) {
        ++r_.outside_fills;
        return;
      }
      AbCell& c = cell(global[inst], arm, *block);
      const bool maker = f.liquidity == Liquidity::Maker;
      ++c.fills;
      c.maker_fills += maker ? 1U : 0U;
      const double px = f.price.to_double();
      const double qty = f.qty.to_double();
      const std::int64_t notional = to_raw(px * qty);
      c.notional_raw += notional;
      const FeeRates& rates = fees.schedule(h->instrument);
      std::int64_t fee =
          static_cast<std::int64_t>(static_cast<double>(notional) *
                                    static_cast<double>(rates.cbps(f.liquidity)) / 1'000'000.0);
      if (maker) fee -= to_raw(px * qty * opt_.maker_rebate_bps / 1e4);
      const double s = f.side == Side::Buy ? qty : -qty;
      const double m0 = mids.at(inst, t);
      for (std::size_t k = 0; k < horizons; ++k) {
        const double mh = mids.at(inst, t + r_.horizons_ns[k]);
        if (m0 <= 0 || mh <= 0) continue;
        const std::int64_t markout = to_raw(s * (mh - px));
        const std::int64_t capture = to_raw(s * (m0 - px));
        AbHorizon& x = c.horizons[k];
        x.all.add(markout, capture, notional);
        (f.side == Side::Buy ? x.buy : x.sell).add(markout, capture, notional);
        x.fees_raw += fee;
      }
    });
    r_.last_ns = std::max(r_.last_ns, last_ns_);
  }

  // Closes what is open, gives each cell its time and the report its arms and comparison.
  void finish() {
    close_all(last_ns_);
    if (!plan_) return;
    const AbPlan& plan = *plan_;
    for (std::size_t g = 0; g < periods_.size(); ++g) {
      for (const Period& p : periods_[g]) {
        if (p.arm == AbPlan::kNoArm || p.end <= p.start) continue;
        if (plan.block_ns() == 0) {
          cell(g, p.arm, 0).ns += p.end - p.start;
          continue;
        }
        for (std::int64_t t = p.start; t < p.end;) {
          const std::int64_t k = plan.block_of(t);
          const std::int64_t until = std::min(p.end, (k + 1) * plan.block_ns());
          cell(g, p.arm, k).ns += until - t;
          t = until;
        }
      }
    }
    for (const auto& [key, i] : cells_) r_.cells.push_back(std::move(pending_[i]));
    const std::size_t horizons = r_.horizons_ns.size();
    for (AbArmSummary& a : r_.arms) a.horizons.assign(horizons, AbHorizon{});
    for (const AbCell& c : r_.cells) {
      AbArmSummary& a = r_.arms[static_cast<std::size_t>(c.arm)];
      a.ns += c.ns;
      a.fills += c.fills;
      a.maker_fills += c.maker_fills;
      a.notional_raw += c.notional_raw;
      for (std::size_t k = 0; k < horizons; ++k) a.horizons[k].add(c.horizons[k]);
    }
    compare(plan);
  }

 private:
  static constexpr std::size_t kUnset = std::numeric_limits<std::size_t>::max();

  void check_same_test(const AbSection& ab, const std::string& path) {
    if (r_.arms.empty()) {
      r_.test = ab.name;
      r_.assignment = ab.assignment;
      r_.seed = ab.seed;
      r_.block_s = ab.block_s;
      for (const AbArmSection& a : ab.arms)
        r_.arms.push_back({a.name, arm_params(a), 0, 0, 0, 0, {}});
      r_.aa = std::all_of(ab.arms.begin(), ab.arms.end(), [&](const AbArmSection& a) {
        return a.params == ab.arms.front().params;
      });
      ab_ = ab;
      return;
    }
    bool same = ab.name == ab_.name && ab.assignment == ab_.assignment && ab.seed == ab_.seed &&
                ab.block_s == ab_.block_s && ab.arms.size() == ab_.arms.size();
    for (std::size_t i = 0; same && i < ab.arms.size(); ++i)
      same = ab.arms[i].name == ab_.arms[i].name && ab.arms[i].params == ab_.arms[i].params;
    if (!same)
      throw std::runtime_error(path + ": its [ab] is not the one of the journals before it (" +
                               ab_.name + ")");
  }

  std::size_t instrument(const std::string& name) {
    const auto it = std::find(r_.instruments.begin(), r_.instruments.end(), name);
    if (it != r_.instruments.end()) return static_cast<std::size_t>(it - r_.instruments.begin());
    r_.instruments.push_back(name);
    periods_.emplace_back();
    open_.push_back(kUnset);
    return r_.instruments.size() - 1;
  }

  void close(std::size_t g, std::int64_t t) {
    if (open_[g] == kUnset) return;
    Period& p = periods_[g][open_[g]];
    p.end = std::max(p.start, t);
    open_[g] = kUnset;
  }
  void close_all(std::int64_t t) {
    for (std::size_t g = 0; g < open_.size(); ++g) close(g, t);
  }
  void move(std::size_t g, int arm, std::int64_t t) {
    close(g, t);
    if (arm == AbPlan::kNoArm) return;
    periods_[g].push_back({t, kNoEnd, arm});
    open_[g] = periods_[g].size() - 1;
  }

  // The block a fill of an order sent under `arm` belongs to: the block of its time in the
  // instrument's last period in that arm, or of that period's end when the instrument has moved
  // on. None when the instrument was never in the arm.
  [[nodiscard]] std::optional<std::int64_t> block_of_fill(std::size_t g,
                                                          int arm,
                                                          std::int64_t t) const {
    const Period* found = nullptr;
    for (const Period& p : periods_[g]) {
      if (p.arm != arm) continue;
      if (p.start <= t || found == nullptr) found = &p;
      if (p.start > t) break;
    }
    if (found == nullptr) return std::nullopt;
    std::int64_t at = std::max(t, found->start);
    if (found->end != kNoEnd && at >= found->end) at = found->end - 1;
    return plan_ ? plan_->block_of(at) : 0;
  }

  AbCell& cell(std::size_t g, int arm, std::int64_t block) {
    const auto key = std::make_tuple(g, arm, block);
    const auto [it, added] = cells_.emplace(key, pending_.size());
    if (added) {
      AbCell c;
      c.instrument = g;
      c.arm = arm;
      c.block = block;
      c.horizons.assign(r_.horizons_ns.size(), AbHorizon{});
      pending_.push_back(std::move(c));
    }
    return pending_[it->second];
  }

  void compare(const AbPlan& plan) {
    const std::size_t arms = r_.arms.size();
    const std::size_t horizons = r_.horizons_ns.size();
    // Pair and cluster of each cell.
    const bool by_block = plan.block_ns() > 0;
    std::map<std::pair<std::size_t, std::int64_t>, std::size_t> pair_of;
    std::vector<std::vector<PairArm>> pairs;
    std::vector<std::int64_t> pair_cluster;
    const auto rounds = static_cast<std::int64_t>(arms);
    for (const AbCell& c : r_.cells) {
      const std::size_t stratum = c.instrument < stratum_.size() && stratum_[c.instrument] != kUnset
                                      ? stratum_[c.instrument]
                                      : 0;
      const std::int64_t round = floor_div(c.block, rounds);
      const auto key =
          by_block ? std::make_pair(c.instrument, round) : std::make_pair(stratum, std::int64_t{0});
      const auto [it, added] = pair_of.emplace(key, pairs.size());
      if (added) {
        PairArm empty;
        empty.net_raw.assign(horizons, 0);
        empty.marked_raw.assign(horizons, 0);
        pairs.emplace_back(arms, empty);
        pair_cluster.push_back(by_block ? round : static_cast<std::int64_t>(stratum));
      }
      PairArm& s = pairs[it->second][static_cast<std::size_t>(c.arm)];
      s.ns += c.ns;
      s.notional_raw += c.notional_raw;
      s.fills += c.fills;
      for (std::size_t k = 0; k < horizons; ++k) {
        s.net_raw[k] += c.horizons[k].net_raw();
        s.marked_raw[k] += c.horizons[k].all.notional_raw;
      }
    }
    std::map<std::int64_t, std::vector<std::size_t>> clusters;
    for (std::size_t p = 0; p < pairs.size(); ++p) {
      const bool complete =
          std::all_of(pairs[p].begin(), pairs[p].end(), [](const PairArm& s) { return s.ns > 0; });
      if (!complete) {
        ++r_.incomplete_pairs;
        continue;
      }
      ++r_.pairs;
      clusters[pair_cluster[p]].push_back(p);
    }
    r_.clusters = clusters.size();
    r_.resamples = opt_.resamples;
    if (r_.pairs == 0) return;
    std::vector<std::vector<std::size_t>> groups;
    groups.reserve(clusters.size());
    for (auto& [k, v] : clusters) groups.push_back(std::move(v));

    // The metrics, as (treated, control) over a draw of clusters (each group once per draw).
    struct Metric {
      std::string name;
      int kind;  // 0 per hour, 1 bps
      std::size_t horizon;
      int what;  // per hour: 0 net, 1 notional, 2 fills
    };
    std::vector<Metric> metrics;
    for (std::size_t k = 0; k < horizons; ++k) {
      metrics.push_back({"net/hour " + horizon_label(r_.horizons_ns[k]), 0, k, 0});
      metrics.push_back({"net bps " + horizon_label(r_.horizons_ns[k]), 1, k, 0});
    }
    metrics.push_back({"notional/hour", 0, 0, 1});
    metrics.push_back({"fills/hour", 0, 0, 2});
    const auto value =
        [&](const Metric& m, const std::vector<std::size_t>& draw, std::size_t arm) -> double {
      if (m.kind == 1) {
        double net = 0;
        double notional = 0;
        for (const std::size_t g : draw) {
          for (const std::size_t p : groups[g]) {
            net += static_cast<double>(pairs[p][arm].net_raw[m.horizon]);
            notional += static_cast<double>(pairs[p][arm].marked_raw[m.horizon]);
          }
        }
        return notional > 0 ? net / notional * 1e4 : std::numeric_limits<double>::quiet_NaN();
      }
      double sum = 0;
      std::size_t n = 0;
      for (const std::size_t g : draw) {
        for (const std::size_t p : groups[g]) {
          const PairArm& s = pairs[p][arm];
          const double hours = static_cast<double>(s.ns) / kNsPerHour;
          const double x = m.what == 0   ? quote(s.net_raw[m.horizon])
                           : m.what == 1 ? quote(s.notional_raw)
                                         : static_cast<double>(s.fills);
          sum += x / hours;
          ++n;
        }
      }
      return n == 0 ? 0.0 : sum / static_cast<double>(n);
    };
    std::vector<std::size_t> all(groups.size());
    for (std::size_t g = 0; g < groups.size(); ++g) all[g] = g;
    std::vector<std::vector<double>> draws(metrics.size() * (arms - 1));
    std::vector<std::size_t> draw(groups.size());
    std::uint64_t state = mix(opt_.seed);
    for (std::size_t b = 0; b < opt_.resamples; ++b) {
      for (std::size_t& g : draw) {
        state = mix(state);
        g = state % groups.size();
      }
      for (std::size_t m = 0; m < metrics.size(); ++m) {
        const double control = value(metrics[m], draw, 0);
        for (std::size_t a = 1; a < arms; ++a) {
          const double d = value(metrics[m], draw, a) - control;
          if (std::isfinite(d)) draws[(a - 1) * metrics.size() + m].push_back(d);
        }
      }
    }
    for (std::size_t a = 1; a < arms; ++a) {
      for (std::size_t m = 0; m < metrics.size(); ++m) {
        AbEffect e;
        e.arm = static_cast<int>(a);
        e.metric = metrics[m].name;
        e.treated = value(metrics[m], all, a);
        e.control = value(metrics[m], all, 0);
        e.diff = e.treated - e.control;
        std::vector<double>& d = draws[(a - 1) * metrics.size() + m];
        if (d.empty()) {
          e.lo = e.hi = std::numeric_limits<double>::quiet_NaN();
        } else {
          std::sort(d.begin(), d.end());
          const auto at = [&](double q) {
            const auto i =
                static_cast<std::size_t>(std::llround(q * static_cast<double>(d.size() - 1)));
            return d[std::min(i, d.size() - 1)];
          };
          e.lo = at(0.025);
          e.hi = at(0.975);
        }
        r_.effects.push_back(std::move(e));
      }
    }
  }

  const AbReportOptions& opt_;
  AbReport& r_;
  AbSection ab_;
  std::optional<AbPlan> plan_;
  std::vector<std::vector<Period>> periods_;
  std::vector<std::size_t> open_;
  std::vector<std::size_t> stratum_;
  std::map<std::tuple<std::size_t, int, std::int64_t>, std::size_t> cells_;
  std::vector<AbCell> pending_;
  std::unordered_map<std::uint64_t, std::uint8_t> order_arm_;
  std::set<std::tuple<std::uint32_t, Side, std::string>> seen_;
  std::int64_t last_ns_ = 0;
};

}  // namespace

void AbHorizon::add(const AbHorizon& o) noexcept {
  const auto sum = [](MarkoutBucket& a, const MarkoutBucket& b) {
    a.markout_raw += b.markout_raw;
    a.capture_raw += b.capture_raw;
    a.notional_raw += b.notional_raw;
    a.fills += b.fills;
  };
  sum(all, o.all);
  sum(buy, o.buy);
  sum(sell, o.sell);
  fees_raw += o.fees_raw;
}

AbReport ab_report(std::span<const std::string> journals, const AbReportOptions& opt) {
  if (journals.empty()) throw std::runtime_error("no journal");
  AbReport r;
  r.horizons_ns = opt.horizons_ns;
  if (r.horizons_ns.empty())
    r.horizons_ns.assign(std::begin(kDefaultMarkoutHorizonsNs),
                         std::end(kDefaultMarkoutHorizonsNs));
  r.maker_rebate_bps = opt.maker_rebate_bps;
  r.journals = journals.size();
  // In the order they ran: the parts of one session (and sessions) by their first event.
  std::vector<Opened> files;
  for (const std::string& path : journals) {
    Opened j;
    j.path = path;
    j.reader = std::make_unique<JournalReader>();
    if (auto res = j.reader->open(path); !res)
      throw std::runtime_error("cannot open " + path + ": " + j.reader->describe(res.error()));
    while (const EventHeader* h = j.reader->next()) {
      if (const std::int64_t t = venue_ts(*h).ns; t != 0) {
        j.first_ns = t;
        break;
      }
    }
    j.reader->reset();
    files.push_back(std::move(j));
  }
  std::stable_sort(files.begin(), files.end(), [](const Opened& a, const Opened& b) {
    return a.first_ns < b.first_ns;
  });
  Reading reading(opt, r);
  std::uint64_t session = 0;
  for (std::size_t i = 0; i < files.size(); ++i) {
    const std::uint64_t id = files[i].reader->header().session_id;
    reading.read(files[i], i != 0 && id == session);
    session = id;
  }
  reading.finish();
  return r;
}

void add_noise_floor(AbReport& r, const AbReport& aa) {
  for (AbEffect& e : r.effects) {
    for (const AbEffect& n : aa.effects) {
      if (n.arm != 1 || n.metric != e.metric || !std::isfinite(n.lo) || !std::isfinite(n.hi))
        continue;
      e.noise = true;
      e.noise_half_width = (n.hi - n.lo) / 2;
    }
  }
}

std::string format_ab_report(const AbReport& r) {
  std::string out;
  auto it = std::back_inserter(out);
  fmt::format_to(it, "test     {}: {}", r.test, r.assignment);
  if (r.block_s > 0) fmt::format_to(it, ", blocks of {} s", r.block_s);
  fmt::format_to(it, ", seed {}, {} arms{}\n", r.seed, r.arms.size(), r.aa ? " (A/A)" : "");
  fmt::format_to(it,
                 "read     {} journal(s), {} to {}\n",
                 r.journals,
                 r.first_ns == 0 ? std::string("-") : format_utc_time(r.first_ns),
                 r.last_ns == 0 ? std::string("-") : format_utc_time(r.last_ns));
  std::uint64_t fills = 0;
  for (const AbArmSummary& a : r.arms) fills += a.fills;
  fmt::format_to(it,
                 "fills    {} in the test, {} of orders sent outside it, {} repeated report(s) "
                 "counted once\n",
                 fills,
                 r.outside_fills,
                 r.repeated_fills);
  fmt::format_to(it,
                 "fees     the configured maker and taker rates{}\n",
                 r.maker_rebate_bps != 0
                     ? fmt::format(", less a maker rebate of {} bps", r.maker_rebate_bps)
                     : std::string());

  fmt::format_to(it,
                 "\n{:<16} {:>10} {:>8} {:>7} {:>16} {:>12}  {}\n",
                 "arm",
                 "inst_hours",
                 "fills",
                 "maker%",
                 "notional",
                 "capture_bps",
                 "params");
  for (const AbArmSummary& a : r.arms) {
    const MarkoutBucket capture = a.horizons.empty() ? MarkoutBucket{} : a.horizons.front().all;
    fmt::format_to(it,
                   "{:<16} {:>10.2f} {:>8} {:>7.1f} {:>16.2f} {:>12.2f}  {}\n",
                   a.name,
                   static_cast<double>(a.ns) / kNsPerHour,
                   a.fills,
                   a.fills == 0
                       ? 0.0
                       : 100.0 * static_cast<double>(a.maker_fills) / static_cast<double>(a.fills),
                   quote(a.notional_raw),
                   capture.capture_bps(),
                   a.params.empty() ? std::string("(configured)") : a.params);
  }
  if (!r.mids) {
    out += "\nno BookTicker stream in the journals: no mid to mark the fills against\n";
  } else {
    fmt::format_to(it,
                   "\n{:<16} {:<8} {:>8} {:>11} {:>8} {:>8} {:>8} {:>8} {:>14}\n",
                   "arm",
                   "horizon",
                   "marked",
                   "markout_bps",
                   "buy",
                   "sell",
                   "fee_bps",
                   "net_bps",
                   "net/hour");
    for (const AbArmSummary& a : r.arms) {
      for (std::size_t k = 0; k < a.horizons.size(); ++k) {
        const AbHorizon& h = a.horizons[k];
        const double marked = static_cast<double>(h.all.notional_raw);
        const double hours = static_cast<double>(a.ns) / kNsPerHour;
        fmt::format_to(
            it,
            "{:<16} {:<8} {:>8} {:>11.2f} {:>8.2f} {:>8.2f} {:>8.2f} {:>8.2f} {:>14.4f}\n",
            a.name,
            horizon_label(r.horizons_ns[k]),
            h.all.fills,
            h.all.markout_bps(),
            h.buy.markout_bps(),
            h.sell.markout_bps(),
            marked > 0 ? static_cast<double>(h.fees_raw) / marked * 1e4 : 0.0,
            marked > 0 ? static_cast<double>(h.net_raw()) / marked * 1e4 : 0.0,
            hours > 0 ? quote(h.net_raw()) / hours : 0.0);
      }
    }
  }
  const std::string_view unit = r.block_s > 0 ? "instrument x run of blocks" : "stratum";
  const std::string_view cluster = r.block_s > 0 ? "runs of blocks" : "strata";
  fmt::format_to(it,
                 "\npaired   against {}: {} pair(s) ({}) in {} cluster(s) ({}), {} incomplete; 95% "
                 "interval from {} resamples of the clusters\n",
                 r.arms.empty() ? std::string("-") : r.arms.front().name,
                 r.pairs,
                 unit,
                 r.clusters,
                 cluster,
                 r.incomplete_pairs,
                 r.resamples);
  if (r.effects.empty()) {
    out += "no pair has time in every arm\n";
    return out;
  }
  const bool noise =
      std::any_of(r.effects.begin(), r.effects.end(), [](const AbEffect& e) { return e.noise; });
  fmt::format_to(it,
                 "{:<16} {:<16} {:>14} {:>14} {:>14} {:>30}{}\n",
                 "arm",
                 "metric",
                 "arm",
                 "control",
                 "diff",
                 "95% interval",
                 noise ? fmt::format(" {:>12}", "A/A +-") : std::string());
  const auto value = [](double v, bool sign) {
    if (!std::isfinite(v)) return std::string("-");
    return sign ? fmt::format("{:+.4f}", v) : fmt::format("{:.4f}", v);
  };
  for (const AbEffect& e : r.effects) {
    fmt::format_to(it,
                   "{:<16} {:<16} {:>14} {:>14} {:>14} {:>30}{}\n",
                   r.arms[static_cast<std::size_t>(e.arm)].name,
                   e.metric,
                   value(e.treated, false),
                   value(e.control, false),
                   value(e.diff, true),
                   std::isfinite(e.lo) && std::isfinite(e.hi)
                       ? fmt::format("[{}, {}]", value(e.lo, true), value(e.hi, true))
                       : std::string("-"),
                   noise ? (e.noise ? fmt::format(" {:>12.4f}", e.noise_half_width)
                                    : fmt::format(" {:>12}", "-"))
                         : std::string());
  }
  return out;
}

std::string ab_report_json(const AbReport& r) {
  std::string out;
  auto it = std::back_inserter(out);
  fmt::format_to(it,
                 "{{\"test\": {}, \"assignment\": {}, \"seed\": {}, \"block_s\": {}, \"aa\": {}, "
                 "\"journals\": {}, \"first_ns\": {}, \"last_ns\": {}, \"mids\": {}, "
                 "\"maker_rebate_bps\": {}, \"outside_fills\": {}, \"repeated_fills\": {}, "
                 "\"pairs\": {}, \"incomplete_pairs\": {}, \"clusters\": {}, \"resamples\": {}, "
                 "\"horizons_ns\": [",
                 json_text(r.test),
                 json_text(r.assignment),
                 r.seed,
                 r.block_s,
                 r.aa ? "true" : "false",
                 r.journals,
                 r.first_ns,
                 r.last_ns,
                 r.mids ? "true" : "false",
                 num(r.maker_rebate_bps),
                 r.outside_fills,
                 r.repeated_fills,
                 r.pairs,
                 r.incomplete_pairs,
                 r.clusters,
                 r.resamples);
  for (std::size_t k = 0; k < r.horizons_ns.size(); ++k)
    fmt::format_to(it, "{}{}", k == 0 ? "" : ", ", r.horizons_ns[k]);
  out += "], \"arms\": [";
  for (std::size_t a = 0; a < r.arms.size(); ++a) {
    const AbArmSummary& s = r.arms[a];
    fmt::format_to(it,
                   "{}{{\"name\": {}, \"params\": {}, \"instrument_hours\": {}, \"fills\": {}, "
                   "\"maker_fills\": {}, \"notional\": {}, \"horizons\": [",
                   a == 0 ? "" : ", ",
                   json_text(s.name),
                   json_text(s.params),
                   num(static_cast<double>(s.ns) / kNsPerHour),
                   s.fills,
                   s.maker_fills,
                   num(quote(s.notional_raw)));
    for (std::size_t k = 0; k < s.horizons.size(); ++k) {
      const AbHorizon& h = s.horizons[k];
      fmt::format_to(it,
                     "{}{{\"horizon_ns\": {}, \"marked\": {}, \"marked_notional\": {}, "
                     "\"capture_bps\": {}, \"markout_bps\": {}, \"buy_markout_bps\": {}, "
                     "\"sell_markout_bps\": {}, \"markout\": {}, \"fees\": {}, \"net\": {}}}",
                     k == 0 ? "" : ", ",
                     r.horizons_ns[k],
                     h.all.fills,
                     num(h.all.notional()),
                     num(h.all.capture_bps()),
                     num(h.all.markout_bps()),
                     num(h.buy.markout_bps()),
                     num(h.sell.markout_bps()),
                     num(h.all.markout_quote()),
                     num(quote(h.fees_raw)),
                     num(quote(h.net_raw())));
    }
    out += "]}";
  }
  out += "], \"effects\": [";
  for (std::size_t i = 0; i < r.effects.size(); ++i) {
    const AbEffect& e = r.effects[i];
    fmt::format_to(it,
                   "{}{{\"arm\": {}, \"metric\": {}, \"arm_value\": {}, \"control\": {}, "
                   "\"diff\": {}, \"lo\": {}, \"hi\": {}, \"aa_half_width\": {}}}",
                   i == 0 ? "" : ", ",
                   json_text(r.arms[static_cast<std::size_t>(e.arm)].name),
                   json_text(e.metric),
                   num(e.treated),
                   num(e.control),
                   num(e.diff),
                   num(e.lo),
                   num(e.hi),
                   e.noise ? num(e.noise_half_width) : std::string("null"));
  }
  out += "]}\n";
  return out;
}

std::string ab_cells_csv(const AbReport& r) {
  std::string out = "instrument,arm,block,hours,fills,maker_fills,notional";
  for (const std::int64_t h : r.horizons_ns) {
    const std::string l = horizon_label(h);
    out += fmt::format(",marked_{0},markout_{0},fees_{0},net_{0}", l);
  }
  out += '\n';
  auto it = std::back_inserter(out);
  for (const AbCell& c : r.cells) {
    fmt::format_to(it,
                   "{},{},{},{},{},{},{}",
                   r.instruments[c.instrument],
                   r.arms[static_cast<std::size_t>(c.arm)].name,
                   c.block,
                   num(static_cast<double>(c.ns) / kNsPerHour),
                   c.fills,
                   c.maker_fills,
                   num(quote(c.notional_raw)));
    for (const AbHorizon& h : c.horizons) {
      fmt::format_to(it,
                     ",{},{},{},{}",
                     h.all.fills,
                     num(h.all.markout_quote()),
                     num(quote(h.fees_raw)),
                     num(quote(h.net_raw())));
    }
    out += '\n';
  }
  return out;
}

}  // namespace fastmm::bt
