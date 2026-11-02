#include "fastmm/backtest/shadow.hpp"

#include "fastmm/backtest/pnl.hpp"
#include "fastmm/backtest/registrations.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/rng.hpp"
#include "fastmm/core/session_state.hpp"
#include "fastmm/core/shadow_status.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/strategies/registry.hpp"

#include <fmt/format.h>

#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>

namespace fastmm::bt {

namespace {

constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
constexpr double kRaw = 1e8;

std::string num(double v) {
  return std::isfinite(v) ? fmt::format("{:.6f}", v) : std::string("null");
}

std::string json_text(std::string_view v) {
  std::string out = "\"";
  for (const char c : v) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

template <std::size_t N>
void copy_text(char (&to)[N], std::string_view from) {
  const std::size_t n = std::min(from.size(), N - 1);
  std::memcpy(to, from.data(), n);
  to[n] = '\0';
}

std::string point_label(const ParamMap& p) {
  if (p.empty()) return "live";
  std::string s;
  for (const auto& [k, v] : p) s.append(s.empty() ? "" : " ").append(k).append("=").append(v);
  return s;
}

// A source the wave holds while the caller keeps the real one, to read its counters.
class Borrowed final : public MdSource {
 public:
  explicit Borrowed(MdSource& s) noexcept : s_(s) {}
  const EventHeader* next() override { return s_.next(); }
  void reset() override { s_.reset(); }
  [[nodiscard]] const sim::RecordedSession* recorded() const override { return s_.recorded(); }
  [[nodiscard]] Timestamp start_ts() const override { return s_.start_ts(); }
  [[nodiscard]] std::string note() const override { return s_.note(); }
  [[nodiscard]] bool order_by_order() const override { return s_.order_by_order(); }
  [[nodiscard]] bool ready() override { return s_.ready(); }

 private:
  MdSource& s_;
};

struct PointWindow {
  double net = 0.0;
  double notional = 0.0;
  std::uint64_t fills = 0;
  std::uint64_t orders = 0;
  void add(const PointWindow& o) noexcept {
    net += o.net;
    notional += o.notional;
    fills += o.fills;
    orders += o.orders;
  }
};

// One instrument over one window, of the session or a point.
struct InstrumentWindow {
  double net = 0.0;
  double notional = 0.0;
  std::uint64_t fills = 0;
  double position = 0.0;
  std::int64_t mid_raw = 0;  // at the window's end
};

struct Window {
  std::int64_t end_ns = 0;
  std::vector<PointWindow> points;
  std::vector<InstrumentWindow> live_point;  // the live point's, per instrument
};

// What the session did: its fills and orders as the scout takes them, each with the first window
// still open then, so the window it counts in does not depend on when the report looks.
struct LiveEvent {
  std::int64_t ts = 0;
  std::uint32_t first_open = 0;
  std::uint32_t instrument = 0;
  bool fill = false;
  Side side = Side::Buy;
  Price price;
  Qty qty;
  Notional fee;
  FeeAsset fee_asset = FeeAsset::Quote;
  bool maker = false;
};

// One closed window as the report keeps it.
struct Closed {
  std::uint32_t session = 0;
  std::int64_t end_ns = 0;
  PointWindow live;
  std::vector<PointWindow> points;
};

struct InstrumentTotals {
  std::string symbol;
  double live_net = 0.0;
  double sim_net = 0.0;
  double live_notional = 0.0;
  double sim_notional = 0.0;
  std::uint64_t live_fills = 0;
  std::uint64_t sim_fills = 0;
  double live_position = 0.0;
  double sim_position = 0.0;
  double sum_error = 0.0;  // of sim_net - live_net per window
  double sum_error2 = 0.0;
  std::uint64_t windows = 0;
};

struct SessionPoint {
  std::string outbound_sha256;
  std::uint64_t outbound_messages = 0;
  std::uint64_t fills = 0;
};

struct SessionRow {
  std::uint64_t session_id = 0;
  std::string path;
  std::uint64_t md_events = 0;
  std::uint64_t parts = 0;
  std::uint64_t late = 0;
  std::uint64_t late_start = 0;
  std::uint64_t seq_gaps = 0;
  bool writer_gone = false;
  bool finished = false;
  std::vector<SessionPoint> points;
};

}  // namespace

std::vector<PairedSum> simultaneous_sums(const std::vector<std::vector<double>>& series,
                                         double level,
                                         std::size_t resamples,
                                         std::uint64_t seed) {
  std::vector<PairedSum> out(series.size());
  if (series.empty()) return out;
  const std::size_t n = series.front().size();
  for (std::size_t v = 0; v < series.size(); ++v) {
    for (const double x : series[v]) out[v].sum += x;
    out[v].lo = out[v].hi = kNan;
  }
  if (n < kMinWindowsForInterval) return out;
  const auto block = std::max<std::size_t>(
      1, static_cast<std::size_t>(std::ceil(std::cbrt(static_cast<double>(n)))));
  const std::size_t blocks = (n + block - 1) / block;
  const double scale = static_cast<double>(n) / static_cast<double>(blocks * block);
  // Sums of a block, in O(1) per series.
  std::vector<std::vector<double>> prefix(series.size(), std::vector<double>(n + 1, 0.0));
  for (std::size_t v = 0; v < series.size(); ++v) {
    for (std::size_t w = 0; w < n; ++w) prefix[v][w + 1] = prefix[v][w] + series[v][w];
  }
  Xoshiro256ss rng(seed);
  const std::size_t count = series.size();
  std::vector<double> draws(count * resamples);
  std::vector<std::size_t> starts(blocks);
  for (std::size_t r = 0; r < resamples; ++r) {
    for (std::size_t& s : starts) s = rng.uniform(n - block + 1);
    for (std::size_t v = 0; v < count; ++v) {
      const std::vector<double>& p = prefix[v];
      double t = 0.0;
      for (const std::size_t s : starts) t += p[s + block] - p[s];
      draws[r * count + v] = t * scale;
    }
  }
  std::vector<double> sd(count, 0.0);
  for (std::size_t v = 0; v < count; ++v) {
    double m = 0.0;
    double m2 = 0.0;
    for (std::size_t r = 0; r < resamples; ++r) {
      const double x = draws[r * count + v] - out[v].sum;
      m += x;
      m2 += x * x;
    }
    m /= static_cast<double>(resamples);
    sd[v] = std::sqrt(std::max(0.0, m2 / static_cast<double>(resamples) - m * m));
  }
  // The largest standardised deviation over the series, per draw: its quantile holds for them all.
  std::vector<double> worst(resamples, 0.0);
  for (std::size_t r = 0; r < resamples; ++r) {
    for (std::size_t v = 0; v < count; ++v) {
      if (sd[v] <= 0.0) continue;
      worst[r] = std::max(worst[r], std::fabs(draws[r * count + v] - out[v].sum) / sd[v]);
    }
  }
  const auto at = std::min(
      resamples - 1, static_cast<std::size_t>(std::ceil(level * static_cast<double>(resamples))));
  std::nth_element(worst.begin(), worst.begin() + static_cast<std::ptrdiff_t>(at), worst.end());
  const double q = worst[at];
  for (std::size_t v = 0; v < count; ++v) {
    out[v].lo = out[v].sum - q * sd[v];
    out[v].hi = out[v].sum + q * sd[v];
  }
  return out;
}

BacktestConfig shadow_config(const JournalReader& prologue,
                             const sim::RecordedSession& recorded,
                             Duration window,
                             std::string_view config_text) {
  const std::string_view text = config_text.empty() ? prologue.config_text() : config_text;
  if (text.empty())
    throw std::runtime_error("shadow: the journal does not embed its configuration");
  Config::LoadOptions lo;
  lo.substitute_env = false;
  lo.allow_inline_secrets = true;  // secrets are never embedded
  BacktestConfig cfg = BacktestConfig::from_config(Config::parse(text, lo, "journal (embedded)"));
  const JournalFileHeader& h = prologue.header();
  cfg.strategy = std::string(h.strategy, strnlen(h.strategy, sizeof h.strategy));
  cfg.measure_wall_clock = false;
  cfg.output_dir.clear();
  cfg.journal_out.clear();
  cfg.config_toml.clear();
  cfg.source.clear();
  cfg.path.clear();
  cfg.engine.state_file.clear();
  cfg.engine.checkpoint_interval = Duration{};
  cfg.keep_orders = false;
  cfg.markout_horizons.clear();
  cfg.equity_bar = window;
  cfg.start_at = Timestamp{};
  cfg.positions.clear();
  cfg.initial_state = InitialState::Journal;
  cfg.params_from_journal = true;
  cfg.controls_from_journal = true;
  cfg.latency_from_journal = false;
  cfg.balances_from_journal = !recorded.balances.empty();
  // What a journal's market data can share a pass under (a journal carries no orders of others).
  cfg.transport.fill_model = sim::FillModel::L2Queue;
  // The instruments and the settings of the session as it ran.
  InstrumentTable table;
  for (const Instrument& i : prologue.instruments()) {
    if (!table.add(i)) throw std::runtime_error("shadow: bad instrument table in the journal");
  }
  if (table.size() != 0) cfg.instruments = table;
  if (cfg.accounting.configured()) cfg.engine.fx = cfg.fx_plan(cfg.instruments);
  if (cfg.underlying.configured()) cfg.engine.underlying = cfg.underlying_plan(cfg.instruments);
  if (prologue.has_session()) {
    cfg.engine.rng_seed = h.rng_seed;
    for (sim::SimVenueConfig& v : cfg.transport.venues)
      v.supports_replace = ((h.replace_venues >> v.venue.value) & 1U) != 0;
    if (cfg.instruments.size() != 0) {
      const std::uint8_t v = cfg.instruments.begin()->venue.value;
      cfg.transport.supports_replace = ((h.replace_venues >> v) & 1U) != 0;
    }
  }
  return cfg;
}

ShadowOptions shadow_options(const ShadowSection& s) {
  ShadowOptions o;
  o.threads = s.threads;
  o.window = seconds(s.window_s);
  o.settle = seconds(s.settle_s);
  o.max_lag = seconds(s.max_lag_s);
  o.report_path = s.report;
  o.status_path = s.status;
  if (!s.grid.empty()) {
    ParamGrid grid;
    for (const auto& [k, values] : s.grid) grid.emplace_back(k, values);
    o.variants = expand_grid(grid);
  }
  for (const auto& p : s.points) o.variants.emplace_back(p.begin(), p.end());
  return o;
}

// ---- the report ------------------------------------------------------------------------------

struct Shadow::Impl {
  explicit Impl(ShadowOptions o) : opt(std::move(o)) {
    labels.emplace_back("live");
    for (const ParamMap& p : opt.variants) labels.push_back(point_label(p));
    stopped_at.assign(labels.size(), kNotStopped);
    points_kept.store(labels.size());
    if (!opt.status_path.empty()) {
      std::string error;
      if (!status_writer.open(opt.status_path, &error))
        FASTMM_LOG_WARN("shadow: status file {}: {}", opt.status_path, error);
      status.pid = static_cast<std::uint32_t>(::getpid());
      status.started_ns = wall_now().ns;
      status.window_ns = opt.window.ns;
      status.settle_ns = opt.settle.ns;
      status.points = static_cast<std::uint32_t>(labels.size());
    }
    if (!opt.report_path.empty()) {
      const std::filesystem::path p(opt.report_path);
      std::error_code ec;
      if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
      std::filesystem::path lines = p;
      lines.replace_extension(".jsonl");
      windows_file = std::fopen(lines.c_str(), "w");
    }
  }
  ~Impl() {
    if (windows_file != nullptr) std::fclose(windows_file);
  }

  static constexpr std::size_t kNotStopped = static_cast<std::size_t>(-1);

  // One session being followed.
  struct Current {
    std::int64_t start_ns = 0;
    std::size_t instruments = 0;
    InstrumentTable table;
    std::unique_ptr<PnLLedger> ledger;
    std::vector<Window> windows;
    std::vector<std::size_t> reported;  // per point, the windows it has reported
    std::vector<bool> over;             // per point, it reports no more
    std::size_t closed = 0;             // windows closed
    double last_live_equity = 0.0;
    std::vector<double> last_live_net;       // per instrument, its net at the last close
    std::vector<std::int64_t> last_mid_raw;  // per instrument
    std::vector<double> live_pos;
    std::deque<LiveEvent> events;
    std::unordered_set<std::string> seen;  // fills by instrument, side and execution
    std::uint64_t before_start = 0;
    std::uint64_t unconverted_fees = 0;
    std::uint32_t row = 0;  // into `sessions`
  };

  [[nodiscard]] std::size_t window_of(std::int64_t ts) const noexcept {
    const std::int64_t d = ts - cur.start_ns;
    return d <= 0 ? 0 : static_cast<std::size_t>((d - 1) / opt.window.ns);
  }

  // One more record the scout has taken (the reading thread).
  void on_record(const EventHeader& h) {
    if (h.recv_ts.ns > scout_ns) scout_ns = h.recv_ts.ns;
    // The windows whose end is `settle` behind the journal are closed: what comes for them now
    // counts in the first one still open.
    if (known_start.load(std::memory_order_acquire)) {
      const std::int64_t d = scout_ns - opt.settle.ns - cur.start_ns;
      if (d > 0) {
        const auto open = static_cast<std::uint32_t>(d / opt.window.ns);
        if (open > first_open.load(std::memory_order_relaxed))
          first_open.store(open, std::memory_order_release);
      }
    }
    const bool outbound = (h.flags & EventHeader::kOutbound) != 0;
    if (outbound) {
      if ((h.flags & EventHeader::kDropped) != 0) return;
      if (h.type != EventType::OutNewOrder && h.type != EventType::OutReplace) return;
      LiveEvent e;
      e.ts = h.recv_ts.ns;
      e.first_open = first_open.load(std::memory_order_relaxed);
      const std::lock_guard lock(mu);
      cur.events.push_back(e);
      return;
    }
    if (h.type != EventType::OrderFill) return;
    const auto& m = msg_cast<OrderFillMsg>(&h);
    if ((m.flags & OrderFillMsg::kUnresolved) != 0) return;
    LiveEvent e;
    e.ts = venue_ts(h).ns;
    e.first_open = first_open.load(std::memory_order_relaxed);
    e.instrument = h.instrument.value;
    e.fill = true;
    e.side = m.side;
    e.price = m.price;
    e.qty = m.qty;
    e.fee = m.fee;
    e.fee_asset = m.fee_asset;
    e.maker = m.liquidity == Liquidity::Maker;
    const std::lock_guard lock(mu);
    if (!m.exec_id.empty()) {
      std::string key =
          fmt::format("{}/{}/{}", h.instrument.value, static_cast<int>(m.side), m.exec_id.view());
      if (!cur.seen.insert(std::move(key)).second) return;
    }
    if (e.ts < session_start_ns) {
      ++cur.before_start;
      return;
    }
    cur.events.push_back(e);
  }

  // A pass has run `index` up to bar `bars` of its rows (under its own thread, the rows its own).
  struct Track {
    std::vector<std::int64_t> last_pnl;
    std::size_t fill_at = 0;
    std::size_t bar_at = 0;
    std::int64_t last_equity = 0;
    std::uint64_t last_orders = 0;
    std::uint64_t fills = 0;
  };

  void consume(std::size_t index, const FillRows& f, const EquityRows& e, std::uint64_t orders) {
    Track& t = tracks[index];
    const std::size_t n_inst = cur.instruments;
    std::vector<InstrumentWindow> rows;
    std::vector<double> nets;
    for (; t.bar_at < e.size(); ++t.bar_at) {
      const std::size_t b = t.bar_at;
      const std::size_t k = window_of(e.ts[b]);
      PointWindow pw;
      rows.assign(index == 0 ? n_inst : 0, InstrumentWindow{});
      nets.assign(n_inst, 0.0);
      for (; t.fill_at < f.size() && f.ts[t.fill_at] <= e.ts[b]; ++t.fill_at) {
        const std::size_t i = t.fill_at;
        const std::uint32_t inst = f.instrument[i];
        if (inst >= n_inst) continue;
        const double notional = cur.table.get(InstrumentId{inst})
                                    .notional(Price::from_raw(f.price[i]), Qty::from_raw(f.qty[i]))
                                    .to_double();
        pw.notional += notional;
        ++pw.fills;
        ++t.fills;
        if (index == 0) {
          rows[inst].notional += notional;
          ++rows[inst].fills;
        }
      }
      const std::int64_t equity = e.equity(b);
      pw.net = static_cast<double>(equity - t.last_equity) / kRaw;
      t.last_equity = equity;
      for (std::size_t i = 0; i < e.by_instrument.size() && i < n_inst; ++i) {
        const InstrumentEquityRows& r = e.by_instrument[i];
        const double net = static_cast<double>(r.pnl[b] - t.last_pnl[i]) / kRaw;
        t.last_pnl[i] = r.pnl[b];
        nets[i] = net;
        if (index == 0) {
          rows[i].net = net;
          rows[i].position = static_cast<double>(r.position[b]) / kRaw;
          rows[i].mid_raw = r.mid[b];
        }
      }
      pw.orders = orders - t.last_orders;
      t.last_orders = orders;
      const std::lock_guard lock(mu);
      Window& w = window(k);
      w.end_ns = std::max(w.end_ns, e.ts[b]);
      w.points[index].add(pw);
      if (index == 0) merge_rows(w.live_point, rows);
      for (std::size_t i = 0; i < n_inst; ++i) point_net[index][i] += nets[i];
      cur.reported[index] = std::max(cur.reported[index], k + 1);
    }
  }

  static void merge_rows(std::vector<InstrumentWindow>& to,
                         const std::vector<InstrumentWindow>& add) {
    if (to.empty()) {
      to = add;
      return;
    }
    for (std::size_t i = 0; i < to.size() && i < add.size(); ++i) {
      to[i].net += add[i].net;
      to[i].notional += add[i].notional;
      to[i].fills += add[i].fills;
      to[i].position = add[i].position;
      to[i].mid_raw = add[i].mid_raw;
    }
  }

  // Under `mu`.
  Window& window(std::size_t k) {
    while (cur.windows.size() <= k) {
      Window w;
      w.points.resize(labels.size());
      w.end_ns = cur.start_ns + static_cast<std::int64_t>(cur.windows.size() + 1) * opt.window.ns;
      cur.windows.push_back(std::move(w));
    }
    return cur.windows[k];
  }

  // Under `mu`. The live side of window k: its events, and the marks the live point saw.
  PointWindow close_live(std::size_t k, const Window& w, std::vector<InstrumentWindow>& rows) {
    PointWindow live;
    rows.assign(cur.instruments, InstrumentWindow{});
    std::deque<LiveEvent> kept;
    for (const LiveEvent& e : cur.events) {
      if (std::max<std::size_t>(window_of(e.ts), e.first_open) > k) {
        kept.push_back(e);
        continue;
      }
      if (!e.fill) {
        ++live.orders;
        continue;
      }
      if (e.instrument >= cur.instruments) continue;
      const Instrument& inst = cur.table.get(InstrumentId{e.instrument});
      Notional fee = e.fee;
      Qty booked = e.qty;
      if (e.fee_asset == FeeAsset::Base) {
        const Qty fee_base = Qty::from_raw(e.fee.raw);
        fee = inst.notional(e.price, fee_base);
        const Qty held = e.side == Side::Buy ? e.qty - fee_base : e.qty + fee_base;
        if (held.raw > 0) booked = held;
      } else if (e.fee_asset == FeeAsset::Other) {
        fee = Notional{};
        ++cur.unconverted_fees;
      }
      cur.ledger->on_fill(InstrumentId{e.instrument}, e.side, e.price, booked, fee);
      const double notional = inst.notional(e.price, e.qty).to_double();
      live.notional += notional;
      ++live.fills;
      rows[e.instrument].notional += notional;
      ++rows[e.instrument].fills;
    }
    cur.events = std::move(kept);
    for (std::size_t i = 0; i < cur.instruments; ++i) {
      if (i < w.live_point.size() && w.live_point[i].mid_raw != 0)
        cur.last_mid_raw[i] = w.live_point[i].mid_raw;
      const InstrumentId id{static_cast<std::uint32_t>(i)};
      if (cur.last_mid_raw[i] != 0) cur.ledger->mark(id, Price::from_raw(cur.last_mid_raw[i]));
      const Position& p = cur.ledger->position(id);
      const double net = static_cast<double>(p.net_pnl().raw) / kRaw;
      rows[i].net = net - cur.last_live_net[i];
      cur.last_live_net[i] = net;
      rows[i].position = static_cast<double>(p.qty.raw) / kRaw;
    }
    const double equity = static_cast<double>(cur.ledger->equity().raw) / kRaw;
    live.net = equity - cur.last_live_equity;
    cur.last_live_equity = equity;
    return live;
  }

  // Closes the windows that every point and the live side have passed. `all`: the session is over.
  bool close_ready(bool all) {
    bool any = false;
    const std::lock_guard lock(mu);
    if (!cur.ledger) return false;
    for (;;) {
      const std::size_t k = cur.closed;
      if (k >= cur.windows.size() && !all) break;
      if (all && k >= cur.windows.size()) break;
      if (!all) {
        if (first_open.load(std::memory_order_acquire) <= k) break;
        bool ready = true;
        for (std::size_t p = 0; p < labels.size(); ++p)
          ready = ready && (cur.over[p] || cur.reported[p] > k);
        if (!ready) break;
      }
      const Window& w = cur.windows[k];
      std::vector<InstrumentWindow> live_rows;
      // The session's last window takes whatever is still to count.
      const PointWindow live = close_live(
          all && k + 1 == cur.windows.size() ? std::numeric_limits<std::size_t>::max() / 2 : k,
          w,
          live_rows);
      Closed c;
      c.session = cur.row;
      c.end_ns = w.end_ns;
      c.live = live;
      c.points = w.points;
      for (std::size_t i = 0; i < cur.instruments; ++i) {
        InstrumentTotals& t = totals[i];
        const InstrumentWindow& l = live_rows[i];
        InstrumentWindow s;
        if (i < w.live_point.size()) s = w.live_point[i];
        t.live_net += l.net;
        t.sim_net += s.net;
        t.live_notional += l.notional;
        t.sim_notional += s.notional;
        t.live_fills += l.fills;
        t.sim_fills += s.fills;
        t.live_position = l.position;
        t.sim_position = s.position;
        const double err = s.net - l.net;
        t.sum_error += err;
        t.sum_error2 += err * err;
        ++t.windows;
      }
      write_window_line(c, live_rows, w.live_point);
      closed.push_back(std::move(c));
      cur.windows[k].live_point.clear();
      ++cur.closed;
      any = true;
    }
    if (any) stale = true;
    return any;
  }

  void write_window_line(const Closed& c,
                         const std::vector<InstrumentWindow>& live_rows,
                         const std::vector<InstrumentWindow>& sim_rows) {
    if (windows_file == nullptr) return;
    std::string nets;
    for (std::size_t p = 1; p < c.points.size(); ++p)
      nets += fmt::format("{}{:.4f}", p == 1 ? "" : ",", c.points[p].net);
    std::string insts;
    for (std::size_t i = 0; i < live_rows.size(); ++i) {
      const InstrumentWindow s = i < sim_rows.size() ? sim_rows[i] : InstrumentWindow{};
      const InstrumentWindow& l = live_rows[i];
      if (l.fills == 0 && s.fills == 0) continue;
      insts += fmt::format(
          "{}{{\"i\":{},\"live_net\":{:.4f},\"sim_net\":{:.4f},\"live_notional\":{:.2f},"
          "\"sim_notional\":{:.2f},\"live_fills\":{},\"sim_fills\":{},"
          "\"live_position\":{:.8g},\"sim_position\":{:.8g}}}",
          insts.empty() ? "" : ",",
          i,
          l.net,
          s.net,
          l.notional,
          s.notional,
          l.fills,
          s.fills,
          l.position,
          s.position);
    }
    fmt::print(
        windows_file,
        "{{\"session\":{},\"end_ns\":{},\"live\":{{\"net\":{:.4f},\"notional\":{:.2f},\"fills\":{},"
        "\"orders\":{}}},\"sim\":{{\"net\":{:.4f},\"notional\":{:.2f},\"fills\":{},\"orders\":{}}},"
        "\"variants_net\":[{}],\"instruments\":[{}]}}\n",
        c.session,
        c.end_ns,
        c.live.net,
        c.live.notional,
        c.live.fills,
        c.live.orders,
        c.points[0].net,
        c.points[0].notional,
        c.points[0].fills,
        c.points[0].orders,
        nets,
        insts);
    std::fflush(windows_file);
  }

  // Under `mu`. The comparison of everything closed so far.
  struct Stats {
    PointWindow live;
    std::vector<PointWindow> points;
    PairedSum error;
    std::vector<PairedSum> diff;  // per point; the live point's stays 0
  };
  [[nodiscard]] Stats compute() const {
    Stats s;
    s.points.resize(labels.size());
    const std::size_t n = closed.size();
    std::vector<std::vector<double>> diffs;
    std::vector<std::size_t> which;
    for (std::size_t p = 1; p < labels.size(); ++p) {
      std::vector<double> d(n, 0.0);
      for (std::size_t w = 0; w < n; ++w) {
        if (w >= stopped_at[p]) continue;  // compared over the windows it ran
        d[w] = closed[w].points[p].net - closed[w].points[0].net;
      }
      diffs.push_back(std::move(d));
      which.push_back(p);
    }
    std::vector<double> err(n, 0.0);
    for (std::size_t w = 0; w < n; ++w) {
      s.live.add(closed[w].live);
      for (std::size_t p = 0; p < labels.size(); ++p) s.points[p].add(closed[w].points[p]);
      err[w] = closed[w].points[0].net - closed[w].live.net;
    }
    s.error = simultaneous_sums({err}).front();
    s.diff.assign(labels.size(), PairedSum{});
    const std::vector<PairedSum> sums = simultaneous_sums(diffs);
    for (std::size_t i = 0; i < which.size(); ++i) s.diff[which[i]] = sums[i];
    return s;
  }

  [[nodiscard]] static ShadowVerdict verdict(const PairedSum& d,
                                             const PairedSum& error,
                                             bool stopped) {
    if (stopped) return ShadowVerdict::Stopped;
    if (!std::isfinite(d.lo)) return ShadowVerdict::Unclear;
    const double margin = std::fabs(error.sum);
    if (d.lo > margin) return ShadowVerdict::Better;
    if (d.hi < -margin) return ShadowVerdict::Worse;
    return ShadowVerdict::Unclear;
  }

  // Rewrites the report and the status file from what is closed. Reporter thread, or the end.
  void publish() {
    std::string json;
    ShadowStatus st;
    {
      const std::lock_guard lock(mu);
      if (stale || report.empty()) {
        stats = compute();
        report = build_report();
        stale = false;
        json = report;
      }
      fill_status(st);
    }
    if (!json.empty() && !opt.report_path.empty()) {
      if (auto w = write_file_atomic(opt.report_path, json); !w)
        FASTMM_LOG_WARN("shadow: report: {}", w.error());
    }
    st.updated_ns = wall_now().ns;
    status_writer.write(st);
  }

  // Under `mu`.
  [[nodiscard]] std::string build_report() const {
    const Stats& s = stats;
    std::string o = "{\n";
    const auto kept = points_kept.load();
    o += fmt::format(
        "  \"schema\": 1,\n  \"strategy\": {},\n  \"window_s\": {},\n  \"settle_s\": {},\n  "
        "\"state\": \"{}\",\n"
        "  \"points\": {},\n  \"points_running\": {},\n  \"windows\": {},\n",
        json_text(strategy),
        opt.window.ns / 1'000'000'000,
        opt.settle.ns / 1'000'000'000,
        to_string(state),
        labels.size(),
        std::min<std::size_t>(kept, labels.size()),
        closed.size());
    // Now: the last window, and the last 15 together.
    PointWindow last_live;
    PointWindow last_sim;
    PointWindow recent_live;
    PointWindow recent_sim;
    if (!closed.empty()) {
      last_live = closed.back().live;
      last_sim = closed.back().points[0];
      for (std::size_t w = closed.size() >= 15 ? closed.size() - 15 : 0; w < closed.size(); ++w) {
        recent_live.add(closed[w].live);
        recent_sim.add(closed[w].points[0]);
      }
    }
    const auto side = [](const PointWindow& p) {
      return fmt::format("{{\"net\": {}, \"notional\": {}, \"fills\": {}, \"orders\": {}}}",
                         num(p.net),
                         num(p.notional),
                         p.fills,
                         p.orders);
    };
    o += fmt::format("  \"error\": {{\n    \"last_window\": {{\"live\": {}, \"sim\": {}}},\n",
                     side(last_live),
                     side(last_sim));
    o += fmt::format("    \"last_15_windows\": {{\"live\": {}, \"sim\": {}}},\n",
                     side(recent_live),
                     side(recent_sim));
    o += fmt::format(
        "    \"all\": {{\"live\": {}, \"sim\": {}, \"net_error\": {}, \"net_error_lo\": {}, "
        "\"net_error_hi\": {}, \"notional_ratio\": {}, \"fills_ratio\": {}}},\n",
        side(s.live),
        s.points.empty() ? side(PointWindow{}) : side(s.points[0]),
        num(s.error.sum),
        num(s.error.lo),
        num(s.error.hi),
        num(s.live.notional != 0.0 && !s.points.empty() ? s.points[0].notional / s.live.notional
                                                        : kNan),
        num(s.live.fills != 0 && !s.points.empty()
                ? static_cast<double>(s.points[0].fills) / static_cast<double>(s.live.fills)
                : kNan));
    o += "    \"by_instrument\": [";
    bool first = true;
    for (const InstrumentTotals& t : totals) {
      if (t.live_fills == 0 && t.sim_fills == 0) continue;
      const double mean = t.windows != 0 ? t.sum_error / static_cast<double>(t.windows) : 0.0;
      const double var =
          t.windows != 0 ? t.sum_error2 / static_cast<double>(t.windows) - mean * mean : 0.0;
      o += fmt::format(
          "{}\n      {{\"symbol\": {}, \"live_net\": {}, \"sim_net\": {}, \"live_notional\": {}, "
          "\"sim_notional\": {}, \"live_fills\": {}, \"sim_fills\": {}, \"live_position\": {}, "
          "\"sim_position\": {}, \"net_error_per_window\": {}, \"net_error_sd\": {}}}",
          first ? "" : ",",
          json_text(t.symbol),
          num(t.live_net),
          num(t.sim_net),
          num(t.live_notional),
          num(t.sim_notional),
          t.live_fills,
          t.sim_fills,
          num(t.live_position),
          num(t.sim_position),
          num(mean),
          num(std::sqrt(std::max(0.0, var))));
      first = false;
    }
    o += first ? "]\n  },\n" : "\n    ]\n  },\n";
    o += "  \"variants\": [";
    for (std::size_t p = 1; p < labels.size(); ++p) {
      const PairedSum& d = s.diff[p];
      const bool over = stopped_at[p] != kNotStopped;
      o += fmt::format(
          "{}\n    {{\"label\": {}, \"net\": {}, \"diff\": {}, \"lo\": {}, \"hi\": {}, "
          "\"verdict\": \"{}\", "
          "\"estimated_live_net\": {}, \"notional\": {}, \"fills\": {}, \"orders\": {}{}}}",
          p == 1 ? "" : ",",
          json_text(labels[p]),
          num(s.points[p].net),
          num(d.sum),
          num(d.lo),
          num(d.hi),
          to_string(verdict(d, s.error, over)),
          num(s.live.net + d.sum),
          num(s.points[p].notional),
          s.points[p].fills,
          s.points[p].orders,
          over ? fmt::format(", \"stopped_at_window\": {}", stopped_at[p]) : std::string());
    }
    o += labels.size() > 1 ? "\n  ],\n" : "],\n";
    // Each point's net per instrument, the live point's first, aligned with "instruments".
    o += "  \"instruments\": [";
    for (std::size_t i = 0; i < totals.size(); ++i)
      o += fmt::format("{}{}", i == 0 ? "" : ", ", json_text(totals[i].symbol));
    o += "],\n  \"net_by_instrument\": [";
    for (std::size_t p = 0; p < point_net.size(); ++p) {
      o += p == 0 ? "\n    [" : ",\n    [";
      for (std::size_t i = 0; i < point_net[p].size(); ++i)
        o += fmt::format("{}{:.4f}", i == 0 ? "" : ", ", point_net[p][i]);
      o += "]";
    }
    o += point_net.empty() ? "],\n" : "\n  ],\n";
    o += "  \"sessions\": [";
    for (std::size_t i = 0; i < sessions.size(); ++i) {
      const SessionRow& r = sessions[i];
      std::string pts;
      for (std::size_t p = 0; p < r.points.size(); ++p) {
        pts += fmt::format(
            "{}{{\"label\": {}, \"outbound_sha256\": \"{}\", \"outbound_messages\": {}, "
            "\"fills\": {}}}",
            p == 0 ? "" : ", ",
            json_text(labels[p]),
            r.points[p].outbound_sha256,
            r.points[p].outbound_messages,
            r.points[p].fills);
      }
      o += fmt::format(
          "{}\n    {{\"session_id\": {}, \"journal\": {}, \"finished\": {}, \"md_events\": {}, "
          "\"parts\": {}, \"late\": {}, \"late_start\": {}, \"seq_gaps\": {}, \"writer_gone\": {}, "
          "\"points\": [{}]}}",
          i == 0 ? "" : ",",
          r.session_id,
          json_text(r.path),
          r.finished,
          r.md_events,
          r.parts,
          r.late,
          r.late_start,
          r.seq_gaps,
          r.writer_gone,
          pts);
    }
    o += sessions.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return o;
  }

  // Under `mu`.
  void fill_status(ShadowStatus& st) {
    st = status;
    const Stats& s = stats;
    st.state = static_cast<std::uint8_t>(state);
    copy_text(st.strategy, strategy);
    st.windows = static_cast<std::uint32_t>(closed.size());
    st.sessions = static_cast<std::uint32_t>(sessions.size());
    st.session_id = sessions.empty() ? 0 : sessions.back().session_id;
    std::size_t stopped_points = 0;
    for (const std::size_t w : stopped_at) stopped_points += w != kNotStopped ? 1 : 0;
    st.stopped_points = static_cast<std::uint32_t>(stopped_points);
    if (const FollowSource* src = source.load(std::memory_order_acquire); src != nullptr) {
      st.reached_ns = src->reached_ns();
      st.lag_ns = st.reached_ns != 0 ? wall_now().ns - st.reached_ns : 0;
      st.events = src->md_events();
      st.late = src->late();
      st.seq_gaps = src->seq_gaps();
    }
    const auto put = [](ShadowStatusSide& to, const PointWindow& p) {
      to.net = p.net;
      to.notional = p.notional;
      to.fills = p.fills;
      to.orders = p.orders;
    };
    if (!s.points.empty()) {
      put(st.live, s.live);
      put(st.sim, s.points[0]);
    }
    st.net_error_lo = s.error.lo;
    st.net_error_hi = s.error.hi;
    if (!closed.empty()) {
      put(st.last_live, closed.back().live);
      put(st.last_sim, closed.back().points[0]);
    }
    // Best first.
    std::vector<std::size_t> order;
    for (std::size_t p = 1; p < s.diff.size(); ++p) order.push_back(p);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
      return s.diff[a].sum != s.diff[b].sum ? s.diff[a].sum > s.diff[b].sum : a < b;
    });
    st.variants = static_cast<std::uint32_t>(std::min(order.size(), kShadowStatusVariants));
    for (std::size_t i = 0; i < st.variants; ++i) {
      const std::size_t p = order[i];
      ShadowStatusVariant& v = st.variant[i];
      copy_text(v.label, labels[p]);
      v.diff = s.diff[p].sum;
      v.lo = s.diff[p].lo;
      v.hi = s.diff[p].hi;
      v.notional = s.points[p].notional;
      v.fills = s.points[p].fills;
      v.verdict =
          static_cast<std::uint8_t>(verdict(s.diff[p], s.error, stopped_at[p] != kNotStopped));
    }
    // The widest gaps.
    std::vector<std::size_t> gaps;
    for (std::size_t i = 0; i < totals.size(); ++i) {
      if (totals[i].live_fills != 0 || totals[i].sim_fills != 0) gaps.push_back(i);
    }
    const auto gap = [&](std::size_t i) {
      return std::fabs(totals[i].sim_net - totals[i].live_net);
    };
    std::sort(gaps.begin(), gaps.end(), [&](std::size_t a, std::size_t b) {
      return gap(a) != gap(b) ? gap(a) > gap(b) : a < b;
    });
    st.instruments = static_cast<std::uint32_t>(std::min(gaps.size(), kShadowStatusInstruments));
    for (std::size_t k = 0; k < st.instruments; ++k) {
      const InstrumentTotals& t = totals[gaps[k]];
      ShadowStatusInstrument& r = st.instrument[k];
      copy_text(r.symbol, t.symbol);
      r.live_net = t.live_net;
      r.sim_net = t.sim_net;
      r.live_notional = t.live_notional;
      r.sim_notional = t.sim_notional;
      r.live_position = t.live_position;
      r.sim_position = t.sim_position;
    }
  }

  // Sheds load when the session pulls away: half the variants, the highest numbered, at each look.
  void check_load() {
    const FollowSource* src = source.load(std::memory_order_acquire);
    if (src == nullptr || opt.max_lag.ns <= 0) return;
    const Timestamp now = steady_now();
    if (look_at.valid() && now - look_at < seconds(30)) return;
    const std::int64_t reached = src->reached_ns();
    const std::int64_t lag = reached != 0 ? wall_now().ns - reached : 0;
    if (look_at.valid() && lag > opt.max_lag.ns) {
      const double rate =
          static_cast<double>(reached - look_reached) / static_cast<double>((now - look_at).ns);
      if (rate < 1.02) {
        const std::size_t kept = points_kept.load();
        const std::size_t next = 1 + (kept - 1) / 2;
        if (next < kept) {
          points_kept.store(next);
          FASTMM_LOG_WARN(
              "shadow: {:.0f} s behind the session at {:.2f} x its pace: {} of {} points keep "
              "running",
              static_cast<double>(lag) / 1e9,
              rate,
              next,
              labels.size());
        }
      }
    }
    look_at = now;
    look_reached = reached;
  }

  ShadowOptions opt;
  std::vector<std::string> labels;
  std::vector<std::size_t> stopped_at;  // per point, the closed windows when it stopped
  std::atomic<std::size_t> points_kept{0};
  mutable std::mutex mu;
  Current cur;
  std::vector<Track> tracks;
  std::atomic<std::uint32_t> first_open{0};
  std::atomic<bool> known_start{false};
  std::int64_t scout_ns = 0;
  std::int64_t session_start_ns = 0;
  std::vector<Closed> closed;
  std::vector<InstrumentTotals> totals;
  std::vector<std::vector<double>> point_net;  // [point][instrument], over the sessions
  std::vector<SessionRow> sessions;
  Stats stats;
  std::string report;
  bool stale = true;
  std::string strategy;
  ShadowState state = ShadowState::Starting;
  std::atomic<const FollowSource*> source{nullptr};
  ShadowStatus status;
  ShadowStatusWriter status_writer;
  std::FILE* windows_file = nullptr;
  Timestamp look_at{};
  std::int64_t look_reached = 0;
};

Shadow::Shadow(ShadowOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
Shadow::~Shadow() = default;

std::string Shadow::report_json() const {
  const std::lock_guard lock(impl_->mu);
  return impl_->report;
}

std::size_t Shadow::windows() const {
  const std::lock_guard lock(impl_->mu);
  return impl_->closed.size();
}

ShadowSessionResult Shadow::session(const std::string& journal_path) {
  Impl& im = *impl_;
  ShadowSessionResult res;
  res.path = journal_path;
  {
    const std::lock_guard lock(im.mu);
    im.cur = Impl::Current{};
    im.state = ShadowState::Starting;
  }
  im.first_open.store(0);
  im.known_start.store(false);
  im.scout_ns = 0;
  im.session_start_ns = 0;
  FollowOptions fo = im.opt.follow;
  fo.settle = im.opt.settle;
  fo.stop = im.opt.stop;
  fo.on_record = [&im](const EventHeader& h) { im.on_record(h); };
  std::unique_ptr<FollowSource> source;
  try {
    source = std::make_unique<FollowSource>(journal_path, fo);
  } catch (const std::runtime_error&) {
    if (im.opt.stop == nullptr || !im.opt.stop->load()) throw;
    res.stopped = true;
    return res;
  }
  res.session_id = source->prologue().header().session_id;
  if (source->empty()) {
    res.empty = true;
    return res;
  }
  BacktestConfig base =
      shadow_config(source->prologue(), *source->recorded(), im.opt.window, im.opt.config_text);
  if (im.opt.configure) im.opt.configure(base);
  const BacktestConfig start = shared_start(base);
  std::vector<BacktestConfig> cfgs;
  cfgs.reserve(im.labels.size());
  cfgs.push_back(point_config(start, ParamMap{}));
  for (const ParamMap& p : im.opt.variants) cfgs.push_back(point_config(start, p));
  const std::size_t threads = static_cast<std::size_t>(std::max(im.opt.threads, 1));
  if (cfgs.size() > threads * kMaxRunsPerPass) {
    throw std::invalid_argument(fmt::format(
        "shadow: {} points need [shadow] threads >= {}: a pass holds {} points, and the session "
        "can be read only once",
        cfgs.size(),
        (cfgs.size() + kMaxRunsPerPass - 1) / kMaxRunsPerPass,
        kMaxRunsPerPass));
  }
  if (const std::string why = multiverse_refusal(cfgs); !why.empty())
    throw std::invalid_argument("shadow: " + why);
  {
    const std::lock_guard lock(im.mu);
    Impl::Current& c = im.cur;
    c.start_ns = source->start_ts().ns;
    c.table = base.instruments;
    c.instruments = c.table.size();
    c.ledger = std::make_unique<PnLLedger>(c.table);
    c.ledger->set_accounting(base.engine.fx);
    c.reported.assign(im.labels.size(), 0);
    c.over.assign(im.labels.size(), false);
    c.last_live_net.assign(c.instruments, 0.0);
    c.last_mid_raw.assign(c.instruments, 0);
    im.session_start_ns = source->prologue().header().start_ts_ns;
    // What the start of the run read before the start time was known: nothing has closed yet.
    std::erase_if(c.events, [&](const LiveEvent& e) { return e.ts < im.session_start_ns; });
    if (im.totals.size() != c.instruments) {
      im.totals.assign(c.instruments, InstrumentTotals{});
      for (std::size_t i = 0; i < c.instruments; ++i) {
        const Instrument& inst = c.table.get(InstrumentId{static_cast<std::uint32_t>(i)});
        im.totals[i].symbol = std::string(inst.symbol.view());
      }
      im.point_net.assign(im.labels.size(), std::vector<double>(c.instruments, 0.0));
    }
    im.strategy = base.strategy;
    SessionRow row;
    row.session_id = res.session_id;
    row.path = journal_path;
    im.sessions.push_back(row);
    c.row = static_cast<std::uint32_t>(im.sessions.size() - 1);
    im.tracks.assign(im.labels.size(), Impl::Track{});
    for (Impl::Track& t : im.tracks) {
      t.last_pnl.assign(c.instruments, 0);
    }
    im.state = ShadowState::CatchingUp;
    im.stale = true;
  }
  im.known_start.store(true, std::memory_order_release);
  im.source.store(source.get(), std::memory_order_release);
  // Reporter: closes windows and publishes, off the threads that step the runs.
  std::atomic<bool> done{false};
  std::jthread reporter([&] {
    Timestamp last = steady_now();
    while (!done.load(std::memory_order_acquire)) {
      sleep_for(milliseconds(50));
      const bool closed_some = im.close_ready(false);
      const Timestamp now = steady_now();
      if (closed_some || now - last >= seconds(1)) {
        {
          const std::lock_guard lock(im.mu);
          const std::int64_t lag = wall_now().ns - source->reached_ns();
          im.state = lag < (im.opt.settle + seconds(30)).ns ? ShadowState::Following
                                                            : ShadowState::CatchingUp;
          im.sessions.back().md_events = source->md_events();
          im.sessions.back().parts = source->parts();
          im.sessions.back().late = source->late();
          im.sessions.back().late_start = source->late_start();
          im.sessions.back().seq_gaps = source->seq_gaps();
        }
        im.publish();
        im.check_load();
        last = now;
      }
    }
  });
  MultiverseWatch watch;
  watch.every = im.opt.window;
  watch.drop = [&im](std::size_t index) { return index != 0 && index >= im.points_kept.load(); };
  watch.tick = [&](Timestamp, std::span<const WatchedRun> runs) {
    for (const WatchedRun& r : runs) {
      if (!r.live) {
        const std::lock_guard lock(im.mu);
        if (!im.cur.over[r.index]) {
          im.cur.over[r.index] = true;
          if (im.stopped_at[r.index] == Impl::kNotStopped)
            im.stopped_at[r.index] = im.closed.size();
        }
        continue;
      }
      const std::uint64_t orders = r.session->orders_sent();
      im.consume(r.index, r.session->fill_rows(), r.session->equity_rows(), orders);
      if (!im.opt.keep_results) {
        Impl::Track& t = im.tracks[r.index];
        const std::size_t forgotten = r.session->forget_rows(t.fill_at, t.bar_at);
        t.fill_at -= forgotten;
        t.bar_at = 0;
      }
    }
  };
  std::vector<SessionPoint> finals(im.labels.size());
  const ResultHook done_hook = [&](std::size_t index, BacktestResult& r) {
    im.consume(index, r.fills, r.equity, r.transport.orders_sent + r.transport.replaces_sent);
    finals[index] = SessionPoint{r.outbound_sha256, r.outbound_messages, 0};
    {
      const std::lock_guard lock(im.mu);
      im.cur.over[index] = true;
    }
    finals[index].fills = im.tracks[index].fills;
  };
  const SourceFactory factory = [&]() -> std::unique_ptr<MdSource> {
    return std::make_unique<Borrowed>(*source);
  };
  try {
    if (im.opt.factory) {
      res.results = run_multiverse(
          cfgs, im.opt.factory, factory, static_cast<int>(threads), done_hook, &watch);
    } else {
      register_builtin_strategies();
      res.results = run_multiverse(
          cfgs, base.strategy, factory, static_cast<int>(threads), done_hook, &watch);
    }
  } catch (...) {
    done.store(true);
    reporter.join();
    im.source.store(nullptr);
    throw;
  }
  done.store(true, std::memory_order_release);
  reporter.join();
  res.md_events = source->md_events();
  res.first_ns = source->start_ts().ns;
  res.last_ns = source->reached_ns();
  res.parts = source->parts();
  res.late = source->late();
  res.late_start = source->late_start();
  res.seq_gaps = source->seq_gaps();
  res.writer_gone = source->writer_gone();
  res.stopped = im.opt.stop != nullptr && im.opt.stop->load();
  im.first_open.store(std::numeric_limits<std::uint32_t>::max());
  im.close_ready(true);
  {
    const std::lock_guard lock(im.mu);
    SessionRow& row = im.sessions.back();
    row.md_events = res.md_events;
    row.parts = res.parts;
    row.late = res.late;
    row.late_start = res.late_start;
    row.seq_gaps = res.seq_gaps;
    row.writer_gone = res.writer_gone;
    row.finished = !res.stopped;
    row.points = finals;
    im.state = ShadowState::Waiting;
    im.stale = true;
    im.status.late += res.late;
  }
  im.source.store(nullptr, std::memory_order_release);
  im.publish();
  return res;
}

namespace {

// <dir>/<name>-<id>.fmj (never a part, x.N.fmj), the id above `after`: the lowest, or with `newest`
// the highest.
std::string find_session(const std::string& dir,
                         const std::string& name,
                         std::uint64_t after,
                         bool newest,
                         std::uint64_t& id) {
  std::error_code ec;
  std::string best;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    const std::string file = e.path().filename().string();
    const std::string prefix = name + "-";
    if (!file.starts_with(prefix) || !file.ends_with(".fmj")) continue;
    const std::string_view digits(file.data() + prefix.size(), file.size() - prefix.size() - 4);
    std::uint64_t v = 0;
    const auto [ptr, err] = std::from_chars(digits.data(), digits.data() + digits.size(), v);
    if (err != std::errc{} || ptr != digits.data() + digits.size() || v <= after) continue;
    if (best.empty() || (newest ? v > id : v < id)) {
      id = v;
      best = e.path().string();
    }
  }
  return best;
}

}  // namespace

void Shadow::follow(const std::string& dir, const std::string& name, Duration idle_exit) {
  Impl& im = *impl_;
  const auto stopped = [&] { return im.opt.stop != nullptr && im.opt.stop->load(); };
  std::uint64_t current = 0;
  bool first = true;
  std::uint64_t id = 0;
  Timestamp idle_since = steady_now();
  im.opt.follow.superseded = [&] {
    std::uint64_t newer = 0;
    return !find_session(dir, name, current, false, newer).empty();
  };
  while (!stopped()) {
    const std::string path = find_session(dir, name, current, first, id);
    if (path.empty()) {
      {
        const std::lock_guard lock(im.mu);
        im.state = first ? ShadowState::Starting : ShadowState::Waiting;
      }
      im.publish();
      if (idle_exit.ns > 0 && steady_now() - idle_since >= idle_exit) break;
      sleep_for(milliseconds(200));
      continue;
    }
    first = false;
    current = id;
    FASTMM_LOG_INFO("shadow: following {}", path);
    try {
      const ShadowSessionResult r = session(path);
      FASTMM_LOG_INFO("shadow: {} ended after {} events", path, r.md_events);
    } catch (const std::exception& e) {
      // A session this build cannot run does not end the shadow: the next one may be fine.
      FASTMM_LOG_ERROR("shadow: {}: {}", path, e.what());
      throw;
    }
    idle_since = steady_now();
  }
  {
    const std::lock_guard lock(im.mu);
    im.state = ShadowState::Stopped;
    im.stale = true;
  }
  im.publish();
}

}  // namespace fastmm::bt
