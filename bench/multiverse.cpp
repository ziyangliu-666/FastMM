// fastmm-bench-multiverse: many variants of a strategy over one pass of recorded data
// (bt::run_multiverse), against the same variants as separate backtests.
//
//   fastmm-bench-multiverse --config C.toml --data SPEC [--strategy NAME]
//                           [--sweep key=v1,v2,...]... [--latency-us a,b,...] [--variants N]
//                           [--own-feed 0|1] [--md-jitter-us J] [--threads T] [--check K] [--solo
//                           K]
//
// The variants are the grid of the --sweep keys (strategy parameters) and --latency-us (the order
// and ack paths' fixed latency), in a fixed shuffled order, the first --variants of them; the first
// variant is the configuration's own. --strategy other than the configuration's runs with that
// strategy's default parameters under the --sweep values. --check K runs K variants, spread over
// the list, through run_backtest() and compares each result with its multiverse run
// (bt::result_difference: every fill, order, equity bar, count and the outbound hash); any
// difference fails the run. --solo K times K separate backtests, the cost the multiverse replaces.
// --own-feed sets [backtest] own_orders_in_feed and --md-jitter-us the market data's latency
// jitter (latency_md_jitter_us) of every variant. The variants keep no order rows, as a sweep's
// points do.
#include "fastmm/backtest/multiverse.hpp"

#include "fastmm/backtest/backtest_config.hpp"
#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/data_registry.hpp"
#include "fastmm/backtest/registrations.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/core/log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace fastmm;

namespace {

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::size_t pos = 0;
  for (;;) {
    const std::size_t c = s.find(',', pos);
    out.push_back(s.substr(pos, c == std::string::npos ? std::string::npos : c - pos));
    if (c == std::string::npos) return out;
    pos = c + 1;
  }
}

double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path;
  std::string data;
  std::string strategy;
  bt::ParamGrid grid;
  std::vector<std::string> latencies;
  std::size_t variants = 0;
  int threads = 1;
  std::size_t check = 0;
  std::size_t solo = 0;
  int own_feed = -1;
  long md_jitter_us = -1;
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
    } else if (a == "--strategy") {
      strategy = next();
    } else if (a == "--sweep") {
      const std::string kv = next();
      const std::size_t eq = kv.find('=');
      if (eq == std::string::npos) {
        std::fprintf(stderr, "--sweep key=v1,v2,...\n");
        return 2;
      }
      grid.emplace_back(kv.substr(0, eq), split(kv.substr(eq + 1)));
    } else if (a == "--latency-us") {
      latencies = split(next());
    } else if (a == "--variants") {
      variants = std::strtoull(next().c_str(), nullptr, 10);
    } else if (a == "--threads") {
      threads = std::atoi(next().c_str());
    } else if (a == "--check") {
      check = std::strtoull(next().c_str(), nullptr, 10);
    } else if (a == "--solo") {
      solo = std::strtoull(next().c_str(), nullptr, 10);
    } else if (a == "--own-feed") {
      own_feed = std::atoi(next().c_str());
    } else if (a == "--md-jitter-us") {
      md_jitter_us = std::atol(next().c_str());
    } else {
      std::fprintf(stderr,
                   "usage: %s --config C.toml --data SPEC [--strategy NAME] [--sweep k=v1,v2]... "
                   "[--latency-us a,b] [--variants N] [--own-feed 0|1] [--md-jitter-us J] "
                   "[--threads T] [--check K] [--solo K]\n",
                   argv[0]);
      return 2;
    }
  }
  if (config_path.empty() || data.empty()) {
    std::fprintf(stderr, "--config and --data are required\n");
    return 2;
  }
  Logger::instance().start(stderr, LogLevel::Error);
  bt::register_builtin_data_sources();
  bt::BacktestConfig base;
  try {
    base = bt::BacktestConfig::from_config(Config::load(config_path));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "config: %s\n", e.what());
    return 2;
  }
  // Another strategy than the configuration's starts from its own defaults.
  if (strategy.empty()) strategy = base.strategy;
  if (strategy != base.strategy) base.params.clear();
  base.output_dir.clear();
  base.journal_out.clear();
  base.keep_orders = false;  // as a sweep's points: the metrics and the hash cover the orders
  if (own_feed >= 0) base.transport.own_orders_in_feed = own_feed != 0;
  if (md_jitter_us >= 0) {
    base.transport.md_in.jitter = microseconds(md_jitter_us);
    for (sim::SimVenueConfig& v : base.transport.venues)
      v.md_in.jitter = base.transport.md_in.jitter;
  }

  // The variants: the configuration's own, then the grid in a fixed shuffled order.
  std::vector<bt::BacktestConfig> cfgs;
  cfgs.push_back(base);
  {
    std::vector<bt::BacktestConfig> all;
    const std::vector<ParamMap> points = bt::expand_grid(grid);
    const std::vector<std::string> lats =
        latencies.empty() ? std::vector<std::string>{""} : latencies;
    for (const ParamMap& p : points) {
      for (const std::string& lat : lats) {
        bt::BacktestConfig c = base;
        for (const auto& [k, v] : p) c.params[k] = v;
        if (!lat.empty()) {
          const Duration d = microseconds(std::strtoll(lat.c_str(), nullptr, 10));
          c.transport.order_out.fixed = d;
          c.transport.ack_in.fixed = d;
        }
        all.push_back(std::move(c));
      }
    }
    std::mt19937_64 rng(12345);
    std::shuffle(all.begin(), all.end(), rng);
    for (bt::BacktestConfig& c : all) {
      if (variants != 0 && cfgs.size() >= variants) break;
      cfgs.push_back(std::move(c));
    }
  }
  const bt::SourceFactory factory = [&] { return bt::open_data(data, &base.instruments); };

  const auto t0 = std::chrono::steady_clock::now();
  std::vector<bt::BacktestResult> mv;
  try {
    mv = bt::run_multiverse(cfgs, strategy, factory, threads);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "multiverse: %s\n", e.what());
    return 2;
  }
  const double mv_s = seconds_since(t0);
  std::uint64_t fills = 0;
  std::uint64_t md = 0;
  for (const bt::BacktestResult& r : mv) {
    fills += r.fills.size();
    md = std::max(md, r.md_events);
  }
  std::printf("%zu variants of %s in one pass on %d thread(s): %.2f s, %.2f ms CPU a variant\n",
              cfgs.size(),
              strategy.c_str(),
              threads,
              mv_s,
              mv_s * 1e3 * threads / static_cast<double>(cfgs.size()));
  std::printf("  %llu fills in all, %llu market-data events a run\n",
              static_cast<unsigned long long>(fills),
              static_cast<unsigned long long>(md));

  // The variants checked and timed alone: spread over the list, the first one included.
  const auto spread = [&](std::size_t k) {
    std::vector<std::size_t> idx;
    for (std::size_t j = 0; j < k && j < cfgs.size(); ++j) idx.push_back(j * cfgs.size() / k);
    return idx;
  };
  int failed = 0;
  const std::size_t alone = std::max(check, solo);
  double solo_s = 0.0;
  std::size_t solo_n = 0;
  for (const std::size_t i : spread(alone)) {
    std::unique_ptr<bt::MdSource> source = factory();
    const auto s0 = std::chrono::steady_clock::now();
    const bt::BacktestResult r = bt::run_backtest(cfgs[i], strategy, source.get());
    solo_s += seconds_since(s0);
    ++solo_n;
    if (check == 0) continue;
    const std::string diff = bt::result_difference(r, mv[i]);
    if (!diff.empty()) {
      ++failed;
      std::printf("  variant %zu differs from its backtest: %s\n", i, diff.c_str());
    }
  }
  if (solo_n > 0) {
    const double each = solo_s / static_cast<double>(solo_n);
    std::printf(
        "  a separate backtest: %.3f s; the multiverse costs %.1f ms CPU a variant, %.0fx less\n",
        each,
        mv_s * 1e3 * threads / static_cast<double>(cfgs.size()),
        each / (mv_s * threads / static_cast<double>(cfgs.size())));
  }
  if (check > 0) {
    std::printf("  %zu variants checked against run_backtest(): %s\n",
                std::min(check, cfgs.size()),
                failed == 0 ? "the same, bit for bit" : "DIFFERENCES");
  }
  return failed == 0 ? 0 : 1;
}
