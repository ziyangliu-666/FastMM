#include "fastmm/backtest/calibrate.hpp"

#include "fastmm/backtest/backtest_config.hpp"
#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/markout.hpp"
#include "fastmm/backtest/own_orders.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/core/messages.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace fastmm::bt {

namespace {

constexpr std::int64_t kMs = 1'000'000;
constexpr double kNsPerUs = 1e3;
// Lognormal excess with sigma 0.5 and mean 1 (sim/latency_model.hpp): its 5th and 50th
// percentiles, exp(-1.645 * 0.5 - 0.125) and exp(-0.125).
constexpr double kExcessP5 = 0.3877;
constexpr double kExcessP50 = 0.8825;

double pct(std::span<const double> sorted, double p) {
  if (sorted.empty()) return 0;
  const double x = p * static_cast<double>(sorted.size() - 1);
  const auto i = static_cast<std::size_t>(x);
  if (i + 1 >= sorted.size()) return sorted.back();
  const double f = x - static_cast<double>(i);
  return sorted[i] + f * (sorted[i + 1] - sorted[i]);
}

double ratio(std::uint64_t a, std::uint64_t b) {
  return b == 0 ? 0.0 : static_cast<double>(a) / static_cast<double>(b);
}

std::vector<FillScore> scores(const FillCheckResult& r) {
  std::vector<FillScore> out;
  for (std::size_t k = 0; k < r.conservatism.size(); ++k)
    out.push_back(FillScore::of(r.summary(k)));
  return out;
}

// fixed + lognormal(mean jitter) through the measured 5th and 50th percentiles.
std::pair<std::int64_t, std::int64_t> fit_leg(std::vector<double> us) {
  if (us.empty()) return {0, 0};
  std::sort(us.begin(), us.end());
  const double p5 = pct(us, 0.05);
  const double p50 = pct(us, 0.5);
  const double jitter = std::max(0.0, (p50 - p5) / (kExcessP50 - kExcessP5));
  const double fixed = std::max(0.0, p5 - kExcessP5 * jitter);
  return {std::llround(fixed), std::llround(jitter)};
}

std::string venue_name(const Config* cfg, VenueId v) {
  if (cfg != nullptr && v.valid() && v.value < cfg->venues.size()) return cfg->venues[v.value].name;
  return fmt::format("venue{}", v.value);
}

std::optional<Config> embedded_config(std::string_view text, const std::string& path) {
  if (text.empty()) return std::nullopt;
  Config::LoadOptions lo;
  lo.substitute_env = false;
  lo.allow_inline_secrets = true;
  try {
    return Config::parse(text, lo, path + " (embedded)");
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::string file_name(const std::string& path) {
  return std::filesystem::path(path).filename().string();
}

std::string num(double v, int dp) {
  return fmt::format("{:.{}f}", v, dp);
}

}  // namespace

// ---- scores -----------------------------------------------------------------------------------

FillScore FillScore::of(const FillCheckSummary& s) noexcept {
  FillScore f;
  f.both = s.both;
  f.live_only = s.live_only;
  f.model_only = s.model_only;
  f.neither = s.neither;
  f.live_qty = s.live_qty;
  f.model_qty = s.model_qty;
  return f;
}

FillScore& FillScore::operator+=(const FillScore& o) noexcept {
  both += o.both;
  live_only += o.live_only;
  model_only += o.model_only;
  neither += o.neither;
  live_qty += o.live_qty;
  model_qty += o.model_qty;
  return *this;
}

double FillScore::hit_rate() const noexcept {
  return ratio(both, live());
}
double FillScore::miss_rate() const noexcept {
  return ratio(live_only, live());
}
double FillScore::false_rate() const noexcept {
  return ratio(model_only, model_only + neither);
}
double FillScore::error() const noexcept {
  return ratio(live_only + model_only, both + live_only + model_only);
}
double FillScore::qty_ratio() const noexcept {
  return live_qty.is_zero()
             ? 0.0
             : static_cast<double>(model_qty.raw) / static_cast<double>(live_qty.raw);
}

Quantiles Quantiles::of(std::vector<double> xs) {
  Quantiles q;
  q.n = xs.size();
  if (xs.empty()) return q;
  std::sort(xs.begin(), xs.end());
  q.p10 = pct(xs, 0.1);
  q.p50 = pct(xs, 0.5);
  q.p90 = pct(xs, 0.9);
  q.p99 = pct(xs, 0.99);
  return q;
}

std::size_t pick_conservatism(std::span<const double> conservatism,
                              std::span<const FillScore> grid) {
  std::size_t best = 0;
  for (std::size_t k = 1; k < grid.size() && k < conservatism.size(); ++k) {
    const double e = grid[k].error();
    const double eb = grid[best].error();
    if (e < eb) {
      best = k;
      continue;
    }
    if (e > eb) continue;
    const double d = std::abs(grid[k].qty_ratio() - 1.0);
    const double db = std::abs(grid[best].qty_ratio() - 1.0);
    if (d < db || (d == db && conservatism[k] > conservatism[best])) best = k;
  }
  return best;
}

// ---- latency ----------------------------------------------------------------------------------

void VenueLatency::fit() {
  const auto [rt_fixed, rt_jitter] = fit_leg(round_trip_us);
  if (!ms_venue_times) {
    std::tie(fixed_us, jitter_us) = fit_leg(to_venue_us);
    // The ack leg is what the round trip leaves.
    ack_us = std::max<std::int64_t>(0, rt_fixed - fixed_us);
    ack_jitter_us = std::max<std::int64_t>(0, rt_jitter - jitter_us);
    return;
  }
  std::vector<double> v = to_venue_us;
  std::sort(v.begin(), v.end());
  fixed_us = std::clamp<std::int64_t>(std::llround(pct(v, 0.5)), 0, rt_fixed);
  jitter_us = 0;
  ack_us = rt_fixed - fixed_us;
  ack_jitter_us = rt_jitter;
}

std::vector<VenueLatency> measure_latency(JournalReader& reader) {
  const std::optional<Config> cfg = embedded_config(reader.config_text(), "journal");
  struct Sent {
    Timestamp at;
    VenueId venue;
    bool acked = false;
    bool timed = false;
  };
  std::unordered_map<std::uint64_t, Sent> sent;
  std::vector<VenueLatency> out;
  std::vector<bool> ms;
  auto slot = [&](VenueId v) -> std::size_t {
    for (std::size_t i = 0; i < out.size(); ++i) {
      if (out[i].venue == v) return i;
    }
    VenueLatency l;
    l.venue = v;
    l.name = venue_name(cfg ? &*cfg : nullptr, v);
    out.push_back(std::move(l));
    ms.push_back(true);
    return out.size() - 1;
  };
  const std::span<const Instrument> instruments = reader.instruments();
  // The order's venue: its header's, else its instrument's.
  const auto venue_of = [&](const EventHeader& h) {
    if (h.venue.valid() || h.instrument.value >= instruments.size()) return h.venue;
    return instruments[h.instrument.value].venue;
  };
  reader.for_each([&](const EventHeader* h) {
    if ((h->flags & EventHeader::kOutbound) != 0) {
      if ((h->flags & EventHeader::kDropped) != 0) return;
      if (h->type == EventType::OutNewOrder) {
        sent[msg_cast<OutNewOrderMsg>(h).cl_ord_id.value] = Sent{h->recv_ts, venue_of(*h)};
      } else if (h->type == EventType::OutReplace) {
        sent[msg_cast<OutReplaceMsg>(h).cl_ord_id.value] = Sent{h->recv_ts, venue_of(*h)};
      }
      return;
    }
    if (h->type != EventType::OrderAck) return;
    const auto it = sent.find(msg_cast<OrderAckMsg>(h).cl_ord_id.value);
    if (it == sent.end()) return;
    Sent& s = it->second;
    const std::size_t i = slot(s.venue);
    if (!s.acked) {
      s.acked = true;
      out[i].round_trip_us.push_back(static_cast<double>((h->recv_ts - s.at).ns) / kNsPerUs);
    }
    if (!s.timed && h->exch_ts.valid()) {
      s.timed = true;
      if (h->exch_ts.ns % kMs != 0) ms[i] = false;
      out[i].to_venue_us.push_back(static_cast<double>((h->exch_ts - s.at).ns) / kNsPerUs);
    }
  });
  reader.reset();
  for (std::size_t i = 0; i < out.size(); ++i) {
    VenueLatency& l = out[i];
    l.ms_venue_times = ms[i] && !l.to_venue_us.empty();
    // A whole-millisecond venue time is on average half a millisecond before the event.
    if (l.ms_venue_times) {
      for (double& x : l.to_venue_us) x += 500.0;
    }
    l.fit();
  }
  return out;
}

// ---- calibration ------------------------------------------------------------------------------

Calibration calibrate(std::span<const std::string> journals, std::span<const double> conservatism) {
  Calibration c;
  for (const double v : conservatism)
    c.conservatism.push_back(static_cast<double>(std::llround(std::clamp(v, 0.0, 1.0) * 1e4)) /
                             1e4);
  for (const std::string& path : journals) {
    SessionCalibration s;
    s.path = path;
    s.name = file_name(path);
    s.check = fill_check(path, c.conservatism);
    s.grid = scores(s.check);
    s.no_touch = scores(fill_check(path, c.conservatism, {.touch = false, .tape = true}));
    s.no_tape = scores(fill_check(path, c.conservatism, {.touch = true, .tape = false}));
    s.no_both = scores(fill_check(path, c.conservatism, {.touch = false, .tape = false}));
    JournalReader reader;
    if (auto r = reader.open(path); !r)
      throw std::runtime_error(fmt::format("cannot open {}: {}", path, to_string(r.error())));
    s.latency = measure_latency(reader);
    c.sessions.push_back(std::move(s));
  }
  const std::size_t n = c.conservatism.size();
  auto pooled = [&](const std::vector<std::size_t>& idx) {
    std::vector<FillScore> g(n);
    for (const std::size_t i : idx) {
      for (std::size_t k = 0; k < n; ++k) g[k] += c.sessions[i].grid[k];
    }
    return g;
  };
  std::vector<std::size_t> all;
  for (std::size_t i = 0; i < c.sessions.size(); ++i) all.push_back(i);
  const std::vector<FillScore> total = pooled(all);
  c.pick = n == 0 ? 0 : pick_conservatism(c.conservatism, total);
  for (const SessionCalibration& s : c.sessions) {
    for (std::size_t k = 1; k < n; ++k) {
      if (s.grid[k].error() != s.grid[0].error() || s.grid[k].model_qty != s.grid[0].model_qty)
        c.identified = true;
    }
  }
  if (c.sessions.size() > 1 && n > 0) {
    for (std::size_t i = 0; i < c.sessions.size(); ++i) {
      CalibrationFold f;
      f.fit = {i};
      for (std::size_t j = 0; j < c.sessions.size(); ++j) {
        if (j != i) f.test.push_back(j);
      }
      f.pick = pick_conservatism(c.conservatism, c.sessions[i].grid);
      f.fit_score = c.sessions[i].grid[f.pick];
      const std::vector<FillScore> held = pooled(f.test);
      f.test_score = held[f.pick];
      f.test_best_pick = pick_conservatism(c.conservatism, held);
      f.test_best = held[f.test_best_pick];
      c.folds.push_back(std::move(f));
    }
  }
  for (const SessionCalibration& s : c.sessions) {
    for (const VenueLatency& v : s.latency) {
      auto it = std::find_if(c.latency.begin(), c.latency.end(), [&](const VenueLatency& l) {
        return l.venue == v.venue;
      });
      if (it == c.latency.end()) {
        VenueLatency l;
        l.venue = v.venue;
        l.name = v.name;
        l.ms_venue_times = true;
        c.latency.push_back(std::move(l));
        it = c.latency.end() - 1;
      }
      it->ms_venue_times = it->ms_venue_times && v.ms_venue_times;
      it->to_venue_us.insert(it->to_venue_us.end(), v.to_venue_us.begin(), v.to_venue_us.end());
      it->round_trip_us.insert(
          it->round_trip_us.end(), v.round_trip_us.begin(), v.round_trip_us.end());
    }
  }
  for (VenueLatency& l : c.latency) l.fit();
  return c;
}

std::vector<std::pair<std::string, std::string>> Calibration::backtest_keys() const {
  std::vector<std::pair<std::string, std::string>> k;
  k.emplace_back("fill_model", "\"l2_queue\"");
  if (!conservatism.empty()) k.emplace_back("queue_conservatism", num(conservatism[pick], 2));
  k.emplace_back("md_arrival", "\"recorded\"");
  const bool per_venue = latency.size() > 1;
  for (const VenueLatency& l : latency) {
    if (l.round_trip_us.empty()) continue;
    const std::string p = per_venue ? fmt::format("venues.{}.", l.name) : std::string();
    k.emplace_back(p + "latency_fixed_us", std::to_string(l.fixed_us));
    k.emplace_back(p + "latency_jitter_us", std::to_string(l.jitter_us));
    k.emplace_back(p + "latency_ack_us", std::to_string(l.ack_us));
    k.emplace_back(p + "latency_ack_jitter_us", std::to_string(l.ack_jitter_us));
  }
  return k;
}

// ---- report -----------------------------------------------------------------------------------

namespace {

std::string ms_text(double ns) {
  return fmt::format("{:.1f}", ns / 1e6);
}

std::string qty_text(double q) {
  return fmt::format("{:.5f}", q);
}

void format_grid_row(std::back_insert_iterator<std::string> it,
                     const std::string& label,
                     const FillScore& s) {
  fmt::format_to(it,
                 "  {:<14} {:>6} {:>6} {:>6} {:>6} {:>6.3f} {:>6.3f} {:>6.3f} {:>6.3f} {:>10.3f}\n",
                 label,
                 s.both,
                 s.live_only,
                 s.model_only,
                 s.neither,
                 s.hit_rate(),
                 s.miss_rate(),
                 s.false_rate(),
                 s.error(),
                 s.qty_ratio());
}

void format_grid_header(std::back_insert_iterator<std::string> it) {
  fmt::format_to(it,
                 "  {:<14} {:>6} {:>6} {:>6} {:>6} {:>6} {:>6} {:>6} {:>6} {:>10}\n",
                 "conservatism",
                 "both",
                 "live",
                 "model",
                 "none",
                 "hit",
                 "miss",
                 "false",
                 "error",
                 "model/live");
}

// Time to fill and queue ahead at the fills for one grid value.
void format_fills(std::back_insert_iterator<std::string> it,
                  const FillCheckResult& r,
                  std::size_t k) {
  std::vector<double> live_ttf;
  std::vector<double> model_ttf;
  std::vector<double> ahead_live;
  std::vector<double> ahead_model;
  std::vector<double> placed;
  std::uint64_t at_zero = 0;
  for (const FillCheckOrder& o : r.orders) {
    if (o.live_first_fill_ts.valid() && o.live_filled.is_positive()) {
      live_ttf.push_back(static_cast<double>((o.live_first_fill_ts - o.ack_ts).ns));
      placed.push_back(o.queue_ahead.to_double());
      if (o.model_ahead_at_live_fill[k].raw >= 0) {
        ahead_live.push_back(o.model_ahead_at_live_fill[k].to_double());
        if (o.model_ahead_at_live_fill[k].is_zero()) ++at_zero;
      }
    }
    if (o.model_first_fill_ts[k].valid()) {
      model_ttf.push_back(static_cast<double>((o.model_first_fill_ts[k] - o.ack_ts).ns));
      if (o.model_ahead_at_fill[k].raw >= 0)
        ahead_model.push_back(o.model_ahead_at_fill[k].to_double());
    }
  }
  const Quantiles lt = Quantiles::of(std::move(live_ttf));
  const Quantiles mt = Quantiles::of(std::move(model_ttf));
  fmt::format_to(
      it,
      "  time to fill (ack to first fill, ms, p10/p50/p90): live {} / {} / {}, model {} / "
      "{} / {}\n",
      ms_text(lt.p10),
      ms_text(lt.p50),
      ms_text(lt.p90),
      ms_text(mt.p10),
      ms_text(mt.p50),
      ms_text(mt.p90));
  const Quantiles qp = Quantiles::of(std::move(placed));
  const Quantiles ql = Quantiles::of(std::move(ahead_live));
  const Quantiles qm = Quantiles::of(std::move(ahead_model));
  fmt::format_to(
      it,
      "  queue ahead (p50/p90): at the ack of orders filled live {} / {}; the model's at "
      "the live fill {} / {} ({} of {} zero); at its own fill {} / {}\n",
      qty_text(qp.p50),
      qty_text(qp.p90),
      qty_text(ql.p50),
      qty_text(ql.p90),
      at_zero,
      ql.n,
      qty_text(qm.p50),
      qty_text(qm.p90));
}

void format_latency(std::back_insert_iterator<std::string> it, const VenueLatency& l) {
  const Quantiles tv = Quantiles::of(l.to_venue_us);
  const Quantiles rt = Quantiles::of(l.round_trip_us);
  fmt::format_to(
      it,
      "  latency {}: {} acks; send to venue time p10/p50/p90 {:.0f} / {:.0f} / {:.0f} us{}; "
      "round trip p10/p50/p90/p99 {:.0f} / {:.0f} / {:.0f} / {:.0f} us\n",
      l.name,
      rt.n,
      tv.p10,
      tv.p50,
      tv.p90,
      l.ms_venue_times ? " (ms venue times, +500)" : "",
      rt.p10,
      rt.p50,
      rt.p90,
      rt.p99);
}

}  // namespace

std::string format_calibration(const Calibration& c) {
  std::string out;
  auto it = std::back_inserter(out);
  const std::size_t n = c.conservatism.size();
  for (const SessionCalibration& s : c.sessions) {
    const FillCheckResult& r = s.check;
    const FillScore live = n == 0 ? FillScore{} : s.grid[0];
    fmt::format_to(
        it,
        "session {}\n  {} orders resting after the ack of {} sent, {} filled live (qty {}); "
        "ties in the ack / end millisecond {} / {}\n",
        s.name,
        r.orders.size(),
        r.orders_sent,
        live.live(),
        qty_text(live.live_qty.to_double()),
        r.ack_ties,
        r.end_ties);
    for (const VenueLatency& l : s.latency) format_latency(it, l);
    if (n == 0) continue;
    format_grid_header(it);
    for (std::size_t k = 0; k < n; ++k) format_grid_row(it, num(c.conservatism[k], 2), s.grid[k]);
    const std::size_t p = c.pick;
    format_grid_row(it, "no ticker", s.no_touch[p]);
    format_grid_row(it, "no tape", s.no_tape[p]);
    format_grid_row(it, "neither", s.no_both[p]);
    format_fills(it, r, p);
    out += '\n';
  }
  if (n == 0) return out;
  if (!c.folds.empty()) {
    std::size_t w = 9;
    for (const SessionCalibration& s : c.sessions) w = std::max(w, s.name.size());
    fmt::format_to(it, "cross-validation: fitted on one session, scored on the others\n");
    fmt::format_to(it,
                   "  {:<{}} {:>6} {:>7} {:>7} {:>7} {:>7} {:>10} {:>12}\n",
                   "fitted on",
                   w,
                   "c",
                   "error",
                   "hit",
                   "miss",
                   "false",
                   "model/live",
                   "best c  err");
    for (const CalibrationFold& f : c.folds) {
      fmt::format_to(
          it,
          "  {:<{}} {:>6.2f} {:>7.3f} {:>7.3f} {:>7.3f} {:>7.3f} {:>10.3f} {:>6.2f} {:>5.3f}\n",
          c.sessions[f.fit.front()].name,
          w,
          c.conservatism[f.pick],
          f.test_score.error(),
          f.test_score.hit_rate(),
          f.test_score.miss_rate(),
          f.test_score.false_rate(),
          f.test_score.qty_ratio(),
          c.conservatism[f.test_best_pick],
          f.test_best.error());
    }
  }
  fmt::format_to(it,
                 "queue_conservatism {:.2f}, fitted on {} session{}{}\n",
                 c.conservatism[c.pick],
                 c.sessions.size(),
                 c.sessions.size() == 1 ? "" : "s",
                 c.identified ? ""
                              : " (not identified: every value scores the same on every session, "
                                "so the most conservative is kept)");
  for (const VenueLatency& l : c.latency) {
    if (l.round_trip_us.empty()) continue;
    fmt::format_to(it,
                   "latency {}: fixed {} us + jitter {} us to the venue, ack {} us + jitter {} us "
                   "back\n",
                   l.name,
                   l.fixed_us,
                   l.jitter_us,
                   l.ack_us,
                   l.ack_jitter_us);
  }
  return out;
}

std::string calibration_snippet(const Calibration& c) {
  std::string out = "[backtest]\n";
  auto it = std::back_inserter(out);
  const auto k = c.backtest_keys();
  for (const auto& [key, value] : k) {
    if (!key.starts_with("venues.")) fmt::format_to(it, "{} = {}\n", key, value);
  }
  std::string current;
  for (const auto& [key, value] : k) {
    if (!key.starts_with("venues.")) continue;
    const std::size_t dot = key.rfind('.');
    const std::string table = key.substr(0, dot);
    if (table != current) {
      fmt::format_to(it, "\n[backtest.{}]\n", table);
      current = table;
    }
    fmt::format_to(it, "{} = {}\n", key.substr(dot + 1), value);
  }
  return out;
}

std::string calibration_csv(const Calibration& c) {
  std::string out =
      "session,inputs,conservatism,both,live_only,model_only,neither,live_qty,model_qty\n";
  auto it = std::back_inserter(out);
  for (const SessionCalibration& s : c.sessions) {
    const std::pair<const char*, const std::vector<FillScore>*> sets[] = {
        {"all", &s.grid},
        {"no_ticker", &s.no_touch},
        {"no_tape", &s.no_tape},
        {"neither", &s.no_both}};
    for (const auto& [name, grid] : sets) {
      for (std::size_t k = 0; k < grid->size(); ++k) {
        const FillScore& f = (*grid)[k];
        fmt::format_to(it,
                       "{},{},{:.4f},{},{},{},{},{},{}\n",
                       s.name,
                       name,
                       c.conservatism[k],
                       f.both,
                       f.live_only,
                       f.model_only,
                       f.neither,
                       f.live_qty.to_double(),
                       f.model_qty.to_double());
      }
    }
  }
  return out;
}

// ---- backtest against live --------------------------------------------------------------------

namespace {

// The journal's BookTicker mids per instrument, in venue time.
class Mids {
 public:
  explicit Mids(JournalReader& reader) {
    reader.for_each([&](const EventHeader* h) {
      if (h->type != EventType::BookTicker || (h->flags & EventHeader::kOutbound) != 0) return;
      const auto& m = msg_cast<BookTickerMsg>(h);
      if (!m.bid_px.is_positive() || !m.ask_px.is_positive()) return;
      const std::uint32_t i = h->instrument.value;
      if (i >= by_inst_.size()) by_inst_.resize(i + 1);
      by_inst_[i].emplace_back(venue_ts(*h).ns, (m.bid_px.to_double() + m.ask_px.to_double()) / 2);
    });
    reader.reset();
    for (auto& v : by_inst_)
      std::stable_sort(
          v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  }
  // The mid at venue time t, 0 before the first and after the last ticker.
  [[nodiscard]] double at(std::uint32_t inst, std::int64_t t) const {
    if (inst >= by_inst_.size() || by_inst_[inst].empty()) return 0;
    const auto& v = by_inst_[inst];
    if (t > v.back().first) return 0;
    auto it = std::upper_bound(
        v.begin(), v.end(), t, [](std::int64_t x, const auto& e) { return x < e.first; });
    if (it == v.begin()) return 0;
    return std::prev(it)->second;
  }
  [[nodiscard]] double last(std::uint32_t inst) const {
    return inst < by_inst_.size() && !by_inst_[inst].empty() ? by_inst_[inst].back().second : 0;
  }

 private:
  std::vector<std::vector<std::pair<std::int64_t, double>>> by_inst_;
};

struct Fill {
  std::int64_t ts = 0;
  std::uint32_t inst = 0;
  bool buy = true;
  double px = 0;
  double qty = 0;
  double fee = 0;  // quote currency
};

TradeSummary summarize(const std::vector<Fill>& fills,
                       const Mids& mids,
                       std::span<const std::int64_t> horizons) {
  TradeSummary t;
  t.markout_bps.assign(horizons.size(), 0.0);
  t.markout_fills.assign(horizons.size(), 0);
  std::vector<double> mo(horizons.size(), 0.0);
  std::vector<double> mo_notional(horizons.size(), 0.0);
  for (const Fill& f : fills) {
    const double s = f.buy ? f.qty : -f.qty;
    ++t.fills;
    if (f.buy) ++t.buys;
    t.qty += f.qty;
    t.notional += f.qty * f.px;
    t.fees += f.fee;
    t.position += s;
    const double end = mids.last(f.inst);
    const double m = mids.at(f.inst, f.ts);
    if (m != 0) {
      t.capture += s * (m - f.px);
      t.drift += s * (end - m);
    } else {
      t.drift += s * (end - f.px);
    }
    for (std::size_t h = 0; h < horizons.size(); ++h) {
      const double mh = mids.at(f.inst, f.ts + horizons[h]);
      if (mh == 0) continue;
      mo[h] += s * (mh - f.px);
      mo_notional[h] += f.qty * f.px;
      ++t.markout_fills[h];
    }
  }
  for (std::size_t h = 0; h < horizons.size(); ++h)
    t.markout_bps[h] = mo_notional[h] > 0 ? mo[h] / mo_notional[h] * 1e4 : 0.0;
  t.net = t.capture + t.drift - t.fees;
  return t;
}

TradeSummary live_summary(const OwnOrderLog& log,
                          const Mids& mids,
                          std::span<const std::int64_t> horizons) {
  std::vector<Fill> fills;
  std::vector<double> ttf;
  for (const OwnOrder& o : log.orders) {
    Timestamp first{};
    for (const OwnFill& f : o.fills) {
      Fill x;
      x.ts = f.at.ts.ns;
      x.inst = o.instrument.value;
      x.buy = o.side == Side::Buy;
      x.px = f.price.to_double();
      x.qty = f.qty.to_double();
      if (f.fee_asset == FeeAsset::Quote) {
        x.fee = f.fee.to_double();
      } else if (f.fee_asset == FeeAsset::Base) {
        x.fee = f.fee.to_double() * x.px;
      }
      fills.push_back(x);
      if (!first.valid() || f.at.ts < first) first = f.at.ts;
    }
    if (first.valid() && o.acked) ttf.push_back(static_cast<double>((first - o.ack.ts).ns));
  }
  std::sort(fills.begin(), fills.end(), [](const Fill& a, const Fill& b) { return a.ts < b.ts; });
  TradeSummary t = summarize(fills, mids, horizons);
  t.orders = log.orders_sent;
  t.time_to_fill_p50_ms = Quantiles::of(std::move(ttf)).p50 / 1e6;
  return t;
}

TradeSummary backtest_summary(const BacktestResult& r,
                              const Mids& mids,
                              std::span<const std::int64_t> horizons) {
  std::vector<Fill> fills;
  const FillRows& f = r.fills;
  for (std::size_t i = 0; i < f.size(); ++i) {
    Fill x;
    x.ts = f.ts[i];
    x.inst = f.instrument[i];
    x.buy = f.side[i] == 0;
    x.px = Price::from_raw(f.price[i]).to_double();
    x.qty = Qty::from_raw(f.qty[i]).to_double();
    x.fee = Notional::from_raw(f.fee[i]).to_double();
    fills.push_back(x);
  }
  TradeSummary t = summarize(fills, mids, horizons);
  t.orders = r.metrics.orders;
  t.time_to_fill_p50_ms = static_cast<double>(r.metrics.fill_quality.time_to_fill_p50_ns) / 1e6;
  return t;
}

using Keys = std::vector<std::pair<std::string, std::string>>;

BacktestResult run_session(const std::string& path,
                           const std::string& text,
                           const std::string& source_name,
                           const Keys& keys,
                           bool live) {
  Config::LoadOptions lo;
  lo.substitute_env = false;
  lo.allow_inline_secrets = true;
  Config cfg = Config::parse(text, lo, source_name);
  for (const auto& [k, v] : keys) {
    // GenericSection keeps TOML scalars as their text, strings unquoted.
    std::string value = v;
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
      value = value.substr(1, value.size() - 2);
    cfg.backtest.values[k] = value;
  }
  BacktestConfig b = BacktestConfig::from_config(cfg);
  b.output_dir.clear();
  b.journal_out.clear();
  b.measure_wall_clock = false;
  if (b.strategy.empty()) throw std::runtime_error(source_name + ": no [strategy] name");
  JournalSource src(path, live);
  return run_backtest(b, b.strategy, &src);
}

}  // namespace

std::vector<BacktestGap> compare_backtests(const Calibration& c,
                                           std::span<const std::string> configs) {
  if (configs.size() > 1 && configs.size() != c.sessions.size())
    throw std::invalid_argument("compare_backtests: one configuration, or one per session");
  const auto read = [](const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
  };
  const std::vector<std::int64_t> horizons(std::begin(kDefaultMarkoutHorizonsNs),
                                           std::end(kDefaultMarkoutHorizonsNs));
  const Keys fitted = c.backtest_keys();
  Keys before;
  for (const auto& kv : fitted) {
    if (kv.first == "fill_model" || kv.first == "md_arrival") before.push_back(kv);
  }
  std::vector<BacktestGap> out;
  for (std::size_t i = 0; i < c.sessions.size(); ++i) {
    const SessionCalibration& s = c.sessions[i];
    JournalReader reader;
    if (auto r = reader.open(s.path); !r)
      throw std::runtime_error(fmt::format("cannot open {}: {}", s.path, to_string(r.error())));
    const std::string config =
        configs.empty() ? std::string() : configs[configs.size() == 1 ? 0 : i];
    const std::string text = config.empty() ? std::string(reader.config_text()) : read(config);
    if (text.empty()) {
      throw std::runtime_error(s.path +
                               " does not embed its configuration; pass the one it ran with");
    }
    const std::string source_name = config.empty() ? fmt::format("{} (embedded)", s.path) : config;
    const bool live = reader.header().tsc0 != 0;
    const OwnOrderLog log = collect_own_orders(reader);
    const Mids mids(reader);
    BacktestGap g;
    g.session = s.name;
    g.horizons_ns = horizons;
    g.live = live_summary(log, mids, horizons);
    g.before =
        backtest_summary(run_session(s.path, text, source_name, before, live), mids, horizons);
    g.after =
        backtest_summary(run_session(s.path, text, source_name, fitted, live), mids, horizons);
    out.push_back(std::move(g));
  }
  return out;
}

std::string format_backtest_gaps(std::span<const BacktestGap> gaps) {
  std::string out;
  auto it = std::back_inserter(out);
  for (const BacktestGap& g : gaps) {
    fmt::format_to(
        it,
        "backtest vs live: {} (strip_own; marked against the journal's BookTicker mids)\n",
        g.session);
    fmt::format_to(it,
                   "  {:<22} {:>12} {:>12} {:>12} {:>12} {:>12}\n",
                   "",
                   "live",
                   "before",
                   "after",
                   "before-live",
                   "after-live");
    auto row = [&](const std::string& name, double l, double b, double a, int dp) {
      fmt::format_to(it,
                     "  {:<22} {:>12} {:>12} {:>12} {:>12} {:>12}\n",
                     name,
                     num(l, dp),
                     num(b, dp),
                     num(a, dp),
                     num(b - l, dp),
                     num(a - l, dp));
    };
    const TradeSummary& l = g.live;
    const TradeSummary& b = g.before;
    const TradeSummary& a = g.after;
    auto d = [](std::uint64_t v) { return static_cast<double>(v); };
    row("orders", d(l.orders), d(b.orders), d(a.orders), 0);
    row("fills", d(l.fills), d(b.fills), d(a.fills), 0);
    row("buys", d(l.buys), d(b.buys), d(a.buys), 0);
    row("qty", l.qty, b.qty, a.qty, 5);
    row("time to fill p50 ms",
        l.time_to_fill_p50_ms,
        b.time_to_fill_p50_ms,
        a.time_to_fill_p50_ms,
        1);
    row("spread capture", l.capture, b.capture, a.capture, 4);
    row("mid drift", l.drift, b.drift, a.drift, 4);
    row("fees", l.fees, b.fees, a.fees, 4);
    row("net", l.net, b.net, a.net, 4);
    row("position", l.position, b.position, a.position, 5);
    for (std::size_t h = 0; h < g.horizons_ns.size(); ++h) {
      row(fmt::format("markout {}s bps", g.horizons_ns[h] / 1'000'000'000),
          l.markout_bps[h],
          b.markout_bps[h],
          a.markout_bps[h],
          3);
    }
    out += '\n';
  }
  return out;
}

}  // namespace fastmm::bt
