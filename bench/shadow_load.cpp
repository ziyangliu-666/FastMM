// fastmm-bench-shadow: what a shadow costs over a recorded session. Reads a finished journal as
// fast as it can (the rate a shadow catches up at), with a built-in strategy in place of the
// session's when it cannot run that one, and prints the cost per point and how many times faster
// than the session's own pace it goes.
//
//   fastmm-bench-shadow --journal x.fmj [--config c.toml] [--strategy basic_mm] [--param k=v]...
//                       [--points N] [--threads T] [--settle-s S] [--window-s W] [--compare]
//
// --compare instead reads the journal twice, as JournalSource and as FollowSource, and prints how
// many events of the two streams differ: the check that `settle` is long enough on this session's
// load.
#include "fastmm/backtest/follow_source.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/shadow.hpp"
#include "fastmm/core/log.hpp"

#include <fmt/format.h>

#include <sys/resource.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace fastmm;

namespace {

long status_kb(const char* key) {
  std::ifstream in("/proc/self/status");
  std::string line;
  const std::size_t n = std::strlen(key);
  while (std::getline(in, line)) {
    if (line.compare(0, n, key) == 0) return std::atol(line.c_str() + n + 1);
  }
  return 0;
}

int compare(const std::string& path, Duration settle) {
  bt::JournalSource whole(path, true);
  bt::FollowOptions fo;
  fo.settle = settle;
  bt::FollowSource follow(path, fo);
  std::uint64_t events = 0;
  std::uint64_t differ = 0;
  std::uint64_t only_one = 0;
  std::int64_t first = 0;
  std::int64_t last = 0;
  std::vector<std::byte> a(kMaxMsgBytes);
  for (;;) {
    const EventHeader* x = whole.next();
    if (x != nullptr) std::memcpy(a.data(), x, x->len);
    const EventHeader* y = follow.next();
    while (y != nullptr && y->type == EventType::Padding) y = follow.next();
    if (x == nullptr && y == nullptr) break;
    if (x == nullptr || y == nullptr) {
      ++only_one;
      break;
    }
    if (first == 0) first = bt::venue_ts(*y).ns;
    last = bt::venue_ts(*y).ns;
    ++events;
    if (std::memcmp(a.data(), y, y->len) != 0) ++differ;
  }
  fmt::print(
      "{} events over {:.0f} s, settle {} s: {} differ ({:.6f}%), {} past the other's end; {} "
      "records "
      "late, {} late for the start, {} sequence gaps\n",
      events,
      static_cast<double>(last - first) / 1e9,
      settle.ns / 1'000'000'000,
      differ,
      100.0 * static_cast<double>(differ) / static_cast<double>(std::max<std::uint64_t>(events, 1)),
      only_one,
      follow.late(),
      follow.late_start(),
      follow.seq_gaps());
  return differ == 0 && only_one == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::string journal;
  std::string strategy;
  std::string config_text;
  std::map<std::string, std::string> params;
  int points = 8;
  int threads = 1;
  int settle_s = 10;
  int window_s = 60;
  bool want_compare = false;
  bool verbose = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto value = [&] {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", a.c_str());
        std::exit(2);
      }
      return std::string(argv[++i]);
    };
    if (a == "--journal") {
      journal = value();
    } else if (a == "--config") {
      std::ifstream in(value());
      std::ostringstream text;
      text << in.rdbuf();
      config_text = text.str();
    } else if (a == "--strategy") {
      strategy = value();
    } else if (a == "--param") {
      const std::string kv = value();
      const std::size_t eq = kv.find('=');
      params[kv.substr(0, eq)] = kv.substr(eq + 1);
    } else if (a == "--points") {
      points = std::atoi(value().c_str());
    } else if (a == "--threads") {
      threads = std::atoi(value().c_str());
    } else if (a == "--settle-s") {
      settle_s = std::atoi(value().c_str());
    } else if (a == "--window-s") {
      window_s = std::atoi(value().c_str());
    } else if (a == "--compare") {
      want_compare = true;
    } else if (a == "--verbose") {
      verbose = true;
    } else {
      std::fprintf(stderr, "unknown argument %s\n", a.c_str());
      return 2;
    }
  }
  if (journal.empty()) {
    std::fprintf(stderr, "--journal is required\n");
    return 2;
  }
  Logger::instance().start(stderr, verbose ? LogLevel::Info : LogLevel::Error);
  Logger::instance().set_level(verbose ? LogLevel::Info : LogLevel::Error);
  if (want_compare) return compare(journal, seconds(settle_s));

  bt::ShadowOptions o;
  o.threads = threads;
  o.window = seconds(window_s);
  o.settle = seconds(settle_s);
  o.max_lag = Duration{};
  o.config_text = config_text;
  // Points that differ the way a sweep's do: the half spread, around the configured one.
  for (int k = 1; k < points; ++k)
    o.variants.push_back({{"half_spread_bps", fmt::format("{:.2f}", 0.5 + 0.25 * k)}});
  o.configure = [&](bt::BacktestConfig& c) {
    if (!strategy.empty()) {
      c.strategy = strategy;
      c.params = params;
      // What the session said to its own strategy and accounts does not apply.
      c.balances_from_journal = false;
      c.controls_from_journal = false;
      c.params_from_journal = false;
    }
  };
  const long rss0 = status_kb("VmRSS:");
  const auto t0 = std::chrono::steady_clock::now();
  bt::Shadow shadow(o);
  const bt::ShadowSessionResult r = shadow.session(journal);
  const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const double span = static_cast<double>(r.last_ns - r.first_ns) / 1e9;
  std::uint64_t orders = 0;
  std::uint64_t fills = 0;
  std::uint64_t rejects = 0;
  for (const bt::BacktestResult& b : r.results) {
    orders += b.transport.orders_sent + b.transport.replaces_sent;
    fills += b.transport.fills;
    rejects += b.engine.risk_rejects;
  }
  fmt::print(
      "{} points on {} threads, {} events over {:.0f} s of session in {:.1f} s of wall time\n",
      points,
      threads,
      r.md_events,
      span,
      wall);
  // CPU time, not wall time: on a machine shared with other work the two part.
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  const auto seconds_of = [](const timeval& t) {
    return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_usec) / 1e6;
  };
  const double user = seconds_of(ru.ru_utime);
  const double sys = seconds_of(ru.ru_stime);
  const double cpu = user + sys;
  fmt::print("  {:.1f} x the session's pace by wall time; {:.1f} x by CPU time per thread\n",
             span / wall,
             span / (cpu / threads));
  fmt::print(
      "  CPU {:.1f} s ({:.1f} user, {:.1f} system): {:.2f} core-seconds per session-hour per "
      "point\n",
      cpu,
      user,
      sys,
      cpu / span * 3600.0 / points);
  fmt::print("  {:.0f} ns of CPU per event per point; {:.0f} events/s through the pass\n",
             cpu * 1e9 / static_cast<double>(r.md_events) / points,
             static_cast<double>(r.md_events) / wall);
  fmt::print("  peak memory {} MB, {:.1f} MB per point above the {} MB at start\n",
             status_kb("VmHWM:") / 1024,
             static_cast<double>(status_kb("VmHWM:") - rss0) / 1024.0 / points,
             rss0 / 1024);
  fmt::print("  points sent {} orders in all, {} fills, {} refused by risk; {} late records\n",
             orders,
             fills,
             rejects,
             r.late);
  if (verbose && !r.results.empty()) {
    const auto& e = r.results.front().engine;
    fmt::print("  engine: {} events, {} book updates, {} orders, {} timers\n",
               e.events,
               e.book_updates,
               e.orders_sent,
               e.timers_fired);
  }
  Logger::instance().stop();
  return 0;
}
