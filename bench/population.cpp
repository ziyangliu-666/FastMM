// fastmm-bench-population: a population of basic_mm variants over one pass of order-by-order data,
// against the engine run once per variant.
//
//   fastmm-bench-population --config C.toml --data SPEC [--lanes N] [--threads T] [--check K]
//                           [--solo K] [--top M]
//
// The lanes are a grid of half_spread_bps, skew_bps_per_unit, quote_qty, max_inventory,
// requote_threshold_ticks and latency (orders and acks alike), taken in a fixed shuffled order; the
// first lane is the configuration's own. --check K runs K lanes, spread over the population,
// through run_backtest() and compares every fill (venue time, instrument, side, price, quantity,
// maker or taker), the positions and the net PnL; any difference fails the run. --solo K times K of
// them as separate backtests, the cost the population replaces.
#include "fastmm/backtest/population.hpp"

#include "fastmm/backtest/backtest_config.hpp"
#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/data_registry.hpp"
#include "fastmm/backtest/registrations.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/sim/venue_order.hpp"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace fastmm;

namespace {

struct LaneDef {
  bt::LaneSpec spec;
  std::string half, skew, qty, inv, thr;
  std::int64_t lat_us = 0;
};

std::vector<LaneDef> make_lanes(const bt::BacktestConfig& cfg, std::size_t n) {
  const char* halfs[] = {"0.25", "0.5", "0.75", "1", "1.5", "2", "3", "4", "6", "8"};
  const char* skews[] = {"0", "0.25", "0.5", "1", "2"};
  const char* qtys[] = {"100", "200", "300", "500"};
  const char* invs[] = {"300", "500", "1000", "2000", "0"};
  const char* thrs[] = {"1", "2", "3"};
  const std::int64_t lats[] = {20, 50, 100, 200, 500};
  std::vector<LaneDef> all;
  for (const char* h : halfs)
    for (const char* s : skews)
      for (const char* q : qtys)
        for (const char* v : invs)
          for (const char* t : thrs)
            for (const std::int64_t lat : lats) {
              LaneDef d;
              d.half = h;
              d.skew = s;
              d.qty = q;
              d.inv = v;
              d.thr = t;
              d.lat_us = lat;
              all.push_back(d);
            }
  std::mt19937_64 rng(12345);
  std::shuffle(all.begin(), all.end(), rng);
  std::vector<LaneDef> out;
  // Lane 0: the configuration's own point.
  LaneDef own;
  own.spec = bt::lane_from_config(cfg);
  const auto get = [&](const char* k, const char* d) {
    const auto it = cfg.params.find(k);
    return it == cfg.params.end() ? std::string(d) : it->second;
  };
  own.half = get("half_spread_bps", "5");
  own.skew = get("skew_bps_per_unit", "1");
  own.qty = get("quote_qty", "0.01");
  own.inv = get("max_inventory", "0.1");
  own.thr = get("requote_threshold_ticks", "1");
  own.lat_us = cfg.transport.order_out.fixed.ns / 1000;
  out.push_back(own);
  for (std::size_t i = 0; out.size() < n; ++i) {
    LaneDef d = all[i % all.size()];
    bt::BacktestConfig c = cfg;
    c.params["half_spread_bps"] = d.half;
    c.params["skew_bps_per_unit"] = d.skew;
    c.params["quote_qty"] = d.qty;
    c.params["max_inventory"] = d.inv;
    c.params["requote_threshold_ticks"] = d.thr;
    d.spec = bt::lane_from_config(c);
    d.spec.order_latency = microseconds(d.lat_us);
    d.spec.ack_latency = microseconds(d.lat_us);
    out.push_back(d);
  }
  return out;
}

bt::BacktestConfig config_of(const bt::BacktestConfig& base, const LaneDef& d) {
  bt::BacktestConfig c = base;
  c.params["half_spread_bps"] = d.half;
  c.params["skew_bps_per_unit"] = d.skew;
  c.params["quote_qty"] = d.qty;
  c.params["max_inventory"] = d.inv;
  c.params["requote_threshold_ticks"] = d.thr;
  c.transport.order_out.fixed = microseconds(d.lat_us);
  c.transport.ack_in.fixed = microseconds(d.lat_us);
  c.output_dir.clear();
  c.journal_out.clear();
  c.markout_horizons.clear();
  return c;
}

double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path;
  std::string data;
  std::size_t lanes = 64;
  int threads = 1;
  std::size_t check = 0;
  std::size_t solo = 0;
  std::size_t top = 10;
  bool drain = false;
  bool kernel = false;   // also run lane_kernel.hpp over the tape and compare
  std::string tape_out;  // write the tape, the lanes' kernel parameters and outputs here
  std::vector<std::string> explicit_lanes;  // "half,skew,qty,inv,thr,lat_us"
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--config") {
      config_path = next();
    } else if (a == "--data") {
      data = next();
    } else if (a == "--lanes") {
      lanes = std::strtoull(next().c_str(), nullptr, 10);
    } else if (a == "--threads") {
      threads = std::atoi(next().c_str());
    } else if (a == "--check") {
      check = std::strtoull(next().c_str(), nullptr, 10);
    } else if (a == "--solo") {
      solo = std::strtoull(next().c_str(), nullptr, 10);
    } else if (a == "--lane") {
      explicit_lanes.push_back(next());
    } else if (a == "--kernel") {
      kernel = true;
    } else if (a == "--tape-out") {
      tape_out = next();
    } else if (a == "--drain") {
      drain = true;
    } else if (a == "--top") {
      top = std::strtoull(next().c_str(), nullptr, 10);
    } else {
      std::fprintf(stderr,
                   "usage: %s --config C.toml --data SPEC [--lanes N] [--threads T] [--check K] "
                   "[--solo K] [--top M]\n",
                   argv[0]);
      return 2;
    }
  }
  if (config_path.empty() || data.empty() || lanes == 0) {
    std::fprintf(stderr, "--config, --data and --lanes > 0 are required\n");
    return 2;
  }
  Logger::instance().start(stderr, LogLevel::Error);
  bt::register_builtin_data_sources();
  bt::BacktestConfig cfg;
  try {
    cfg = bt::BacktestConfig::from_config(Config::load(config_path));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "config: %s\n", e.what());
    return 2;
  }
  cfg.output_dir.clear();
  cfg.markout_horizons.clear();
  std::vector<LaneDef> defs;
  if (explicit_lanes.empty()) {
    defs = make_lanes(cfg, lanes);
  } else {
    for (const std::string& e : explicit_lanes) {
      std::vector<std::string> f;
      std::size_t pos = 0;
      for (;;) {
        const std::size_t c = e.find(',', pos);
        f.push_back(e.substr(pos, c == std::string::npos ? std::string::npos : c - pos));
        if (c == std::string::npos) break;
        pos = c + 1;
      }
      if (f.size() != 6) {
        std::fprintf(stderr, "--lane half,skew,qty,inv,thr,lat_us\n");
        return 2;
      }
      LaneDef d;
      d.half = f[0];
      d.skew = f[1];
      d.qty = f[2];
      d.inv = f[3];
      d.thr = f[4];
      d.lat_us = std::atoll(f[5].c_str());
      bt::BacktestConfig c = cfg;
      c.params["half_spread_bps"] = d.half;
      c.params["skew_bps_per_unit"] = d.skew;
      c.params["quote_qty"] = d.qty;
      c.params["max_inventory"] = d.inv;
      c.params["requote_threshold_ticks"] = d.thr;
      d.spec = bt::lane_from_config(c);
      d.spec.order_latency = microseconds(d.lat_us);
      d.spec.ack_latency = microseconds(d.lat_us);
      defs.push_back(d);
    }
  }
  std::vector<bt::LaneSpec> specs;
  for (const LaneDef& d : defs) specs.push_back(d.spec);
  const auto factory = [&]() { return bt::open_data(data, &cfg.instruments); };

  if (drain) {
    // What reading the data alone costs: the source, in venue order.
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_ptr<bt::MdSource> src = factory();
    sim::VenueOrderSource ordered(*src, cfg.reorder_window);
    std::uint64_t n = 0;
    while (ordered.next() != nullptr) ++n;
    std::printf("drain: %" PRIu64 " events in %.2f s\n", n, seconds_since(t0));
  }
  // The population.
  bt::PopulationStats st;
  std::vector<std::vector<bt::LaneFill>> fills;
  std::vector<bt::LaneResult> res;
  try {
    res = bt::run_population(cfg, specs, factory, threads, &st, check > 0 ? &fills : nullptr);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "population: %s\n", e.what());
    return 1;
  }
  std::uint64_t total_fills = 0;
  for (const bt::LaneResult& r : res) total_fills += r.fills;
  std::printf("population: %zu lanes, %d threads, %.2f s wall, %.1f ms per lane\n",
              res.size(),
              threads,
              st.seconds,
              1000.0 * st.seconds / static_cast<double>(res.size()));
  std::printf("  source events %" PRIu64 ", md events %" PRIu64 ", lane wakes %" PRIu64
              ", requotes %" PRIu64 ", fills %" PRIu64 "\n",
              st.source_events,
              st.md_events,
              st.lane_wakes,
              st.lane_requotes,
              total_fills);
  std::vector<std::size_t> order(res.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return res[a].net_pnl.raw > res[b].net_pnl.raw;
  });
  std::printf("  %-6s %6s %5s %5s %5s %4s %6s %12s %8s %8s %10s\n",
              "lane",
              "half",
              "skew",
              "qty",
              "inv",
              "thr",
              "lat_us",
              "net_pnl",
              "fills",
              "taker",
              "replaces");
  const auto row = [&](std::size_t i) {
    const LaneDef& d = defs[i];
    const bt::LaneResult& r = res[i];
    std::printf("  %-6zu %6s %5s %5s %5s %4s %6" PRId64 " %12.2f %8" PRIu64 " %8" PRIu64
                " %10" PRIu64 "\n",
                i,
                d.half.c_str(),
                d.skew.c_str(),
                d.qty.c_str(),
                d.inv.c_str(),
                d.thr.c_str(),
                d.lat_us,
                r.net_pnl.to_double(),
                r.fills,
                r.taker_fills,
                r.replaces);
  };
  for (std::size_t k = 0; k < std::min(top, order.size()); ++k) row(order[k]);
  if (order.size() > top) {
    std::printf("  ...\n");
    row(order.back());
  }

  // The same lanes through lane_kernel.hpp, each (lane, instrument) over the compiled tape.
  if (kernel || !tape_out.empty()) {
    auto t0 = std::chrono::steady_clock::now();
    std::unique_ptr<bt::MdSource> src = factory();
    const bt::Tape tape = bt::compile_tape(cfg, *src);
    std::size_t nrec = 0;
    for (const auto& r : tape.recs) nrec += r.size();
    std::printf(
        "tape: %zu records, %zu level tables of %d levels a side, %zu arrivals, compiled "
        "in %.2f s\n",
        nrec,
        tape.levels.size(),
        fastmm::lk::kLevels,
        tape.adds.size(),
        seconds_since(t0));
    t0 = std::chrono::steady_clock::now();
    const std::vector<fastmm::lk::Out> ko = bt::run_tape(tape, specs, threads);
    const double ks = seconds_since(t0);
    const std::size_t ni = tape.insts.size();
    std::size_t differ = 0;
    std::size_t errors = 0;
    for (std::size_t i = 0; i < res.size(); ++i) {
      Notional cash{};
      Notional fee{};
      std::uint64_t nf = 0;
      std::uint64_t rp = 0;
      bool same_pos = true;
      for (std::size_t k = 0; k < ni; ++k) {
        const fastmm::lk::Out& o = ko[i * ni + k];
        cash = cash + Notional::from_raw(o.cash);
        fee = fee + Notional::from_raw(o.fees);
        nf += o.fills;
        rp += o.replaces;
        same_pos = same_pos && o.position == res[i].positions[k].raw;
        errors += o.error != 0 ? 1 : 0;
      }
      if (cash != res[i].cash || fee != res[i].fees || nf != res[i].fills ||
          rp != res[i].replaces || !same_pos) {
        if (differ < 5) {
          std::printf("  differs: lane %zu (%s,%s,%s,%s,%s,%" PRId64 ") fills %" PRIu64
                      " vs %" PRIu64 " replaces %" PRIu64 " vs %" PRIu64 ", errors",
                      i,
                      defs[i].half.c_str(),
                      defs[i].skew.c_str(),
                      defs[i].qty.c_str(),
                      defs[i].inv.c_str(),
                      defs[i].thr.c_str(),
                      defs[i].lat_us,
                      nf,
                      res[i].fills,
                      rp,
                      res[i].replaces);
          for (std::size_t k = 0; k < ni; ++k) std::printf(" %u", ko[i * ni + k].error);
          std::printf("\n");
        }
        ++differ;
      }
    }
    std::printf(
        "kernel: %zu lanes x %zu instruments on %d threads in %.2f s (%.1f ms per lane), "
        "%zu lanes differ from the population, %zu units hit the tape's limits\n",
        res.size(),
        ni,
        threads,
        ks,
        1000.0 * ks / static_cast<double>(res.size()),
        differ,
        errors);
    if (!tape_out.empty()) {
      tape.save(tape_out + ".tape");
      std::FILE* f = std::fopen((tape_out + ".lanes").c_str(), "wb");
      const std::uint64_t n = specs.size();
      std::fwrite(&n, sizeof n, 1, f);
      for (const bt::LaneSpec& sp : specs) {
        const fastmm::lk::Params p = bt::kernel_params(sp);
        std::fwrite(&p, sizeof p, 1, f);
      }
      std::fwrite(ko.data(), sizeof(fastmm::lk::Out), ko.size(), f);
      std::fclose(f);
      std::printf("wrote %s.tape and %s.lanes\n", tape_out.c_str(), tape_out.c_str());
    }
    if (differ != 0) return 1;
  }

  // Solo backtests of some lanes: what each costs alone.
  if (solo > 0) {
    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t n = std::min(solo, res.size());
    for (std::size_t k = 0; k < n; ++k) {
      const std::size_t i = k * res.size() / n;
      const bt::BacktestConfig c = config_of(cfg, defs[i]);
      std::unique_ptr<bt::MdSource> src = factory();
      static_cast<void>(bt::run_backtest(c, "basic_mm", src.get()));
    }
    const double s = seconds_since(t0);
    std::printf(
        "solo: %zu backtests, %.2f s, %.1f ms each; population per lane is %.0fx less "
        "(per thread %.0fx)\n",
        n,
        s,
        1000.0 * s / static_cast<double>(n),
        (s / static_cast<double>(n)) / (st.seconds / static_cast<double>(res.size())),
        (s / static_cast<double>(n)) / (st.seconds * threads / static_cast<double>(res.size())));
  }

  // Exact check against the engine.
  int failures = 0;
  if (check > 0) {
    const std::size_t n = std::min(check, res.size());
    for (std::size_t k = 0; k < n; ++k) {
      const std::size_t i = k * res.size() / n;
      const bt::BacktestConfig c = config_of(cfg, defs[i]);
      std::unique_ptr<bt::MdSource> src = factory();
      const bt::BacktestResult r = bt::run_backtest(c, "basic_mm", src.get());
      const bt::FillRows& f = r.fills;
      const std::vector<bt::LaneFill>& g = fills[i];
      std::size_t first = std::min(f.size(), g.size());
      for (std::size_t j = 0; j < std::min(f.size(), g.size()); ++j) {
        if (f.ts[j] != g[j].ts.ns || f.instrument[j] != g[j].instrument ||
            f.side[j] != static_cast<std::int8_t>(g[j].side) || f.price[j] != g[j].price.raw ||
            f.qty[j] != g[j].qty.raw ||
            f.liquidity[j] != static_cast<std::uint8_t>(g[j].liquidity)) {
          first = j;
          break;
        }
      }
      const bool same_fills = f.size() == g.size() && first == f.size();
      // The engine's fills, booked as cash: what they paid and received, their fees and the
      // positions they leave. The engine's own net PnL books each fill through an average cost,
      // rounded to the fixed-point unit at every fill, so it is shown beside it, not compared.
      Notional cash{};
      Notional fee{};
      std::vector<Qty> pos(cfg.instruments.size());
      for (std::size_t j = 0; j < f.size(); ++j) {
        const Price px = Price::from_raw(f.price[j]);
        const Qty q = Qty::from_raw(f.qty[j]);
        const Notional v = cfg.instruments.get(InstrumentId{f.instrument[j]}).notional(px, q);
        cash = f.side[j] == 0 ? cash - v : cash + v;
        fee = fee + Notional::from_raw(f.fee[j]);
        pos[f.instrument[j]] += f.side[j] == 0 ? q : -q;
      }
      const bool same_book = cash == res[i].cash && fee == res[i].fees && pos == res[i].positions;
      // The run's decomposition of its fills at the final mid; the engine's own net PnL goes
      // through an average cost rounded at every fill (the decomposition's residual).
      const double ref_pnl = r.metrics.decomposition.net;
      const double pnl = res[i].net_pnl.to_double();
      std::printf(
          "check lane %zu: engine %zu fills net %.4f, population %zu fills net %.4f "
          "(cash, fees, positions %s): %s\n",
          i,
          f.size(),
          ref_pnl,
          g.size(),
          pnl,
          same_book ? "equal" : "differ",
          same_fills && same_book ? "same" : "DIFFERENT");
      const bool same_pnl = same_book;
      if (!same_fills) {
        ++failures;
        const auto show_f = [&](std::size_t j) {
          std::printf("    engine     #%zu ts %" PRId64 " inst %u side %d px %" PRId64
                      " qty %" PRId64 " liq %u\n",
                      j,
                      f.ts[j],
                      f.instrument[j],
                      f.side[j],
                      f.price[j],
                      f.qty[j],
                      f.liquidity[j]);
        };
        const auto show_g = [&](std::size_t j) {
          std::printf("    population #%zu ts %" PRId64 " inst %u side %d px %" PRId64
                      " qty %" PRId64 " liq %u\n",
                      j,
                      g[j].ts.ns,
                      g[j].instrument,
                      static_cast<int>(g[j].side),
                      g[j].price.raw,
                      g[j].qty.raw,
                      static_cast<unsigned>(g[j].liquidity));
        };
        for (std::size_t j = first > 2 ? first - 2 : 0; j < first + 3; ++j) {
          if (j < f.size()) show_f(j);
          if (j < g.size()) show_g(j);
        }
      } else if (!same_pnl) {
        ++failures;
      }
    }
    std::printf("check: %zu lanes, %d different\n", n, failures);
  }
  Logger::instance().stop();
  return failures == 0 ? 0 : 1;
}
