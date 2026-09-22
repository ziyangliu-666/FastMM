// Run-to-completion sessions ([engine] threading = "single") against the in-process
// Binance-compatible simulator: fastmm::live::run_live with BasicMM runs the venue's reactor, the
// engine and order sending on one thread (no fm-net-* thread), trades over WebSocket and over
// WebSocket + TLS, shuts down with cancel_all, and its journal replays to the identical outbound
// stream. The nasdaq_itch venue in this mode is covered by integration.nasdaq_itch_processes_single
// (scripts/bench-e2e.sh --threading single --replay).
#include "integration_util.hpp"

#include "fastmm/backtest/replay.hpp"
#include "fastmm/core/status_segment.hpp"
#include "fastmm/live/session.hpp"
#include "fastmm/strategies/builtin.hpp"
#include "fastmm/strategies/registry.hpp"

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>

using namespace fastmm;
using namespace fastmm::integration;

namespace {

std::string fresh(const std::string& name) {
  const auto p = fastmm::test::tmp_dir() / name;
  std::filesystem::remove(p);
  return p.string();
}

void register_strategies_once() {
  static const bool done = [] {
    register_builtin_strategies(StrategyRegistry::instance());
    return true;
  }();
  static_cast<void>(done);
}

std::set<std::string> thread_names() {
  std::set<std::string> names;
  for (const auto& e : std::filesystem::directory_iterator("/proc/self/task")) {
    std::ifstream in(e.path() / "comm");
    std::string name;
    if (std::getline(in, name)) names.insert(name);
  }
  return names;
}

struct SessionResult {
  int rc = -1;
  bool traded = false;
  std::set<std::string> threads;  // while trading
};

// Runs BasicMM until the simulator has seen `fills` fills, then stops the session like SIGTERM.
SessionResult run_single(ServerFixture& fx,
                         bool tls,
                         const std::string& name,
                         std::uint64_t fills) {
  register_strategies_once();
  Config cfg = sim_local_config(fx, tls);
  cfg.engine.name = name;
  cfg.engine.threading = "single";
  cfg.engine.epoch_file = fresh(name + ".epoch");
  live::LiveOptions o;
  o.duration_ns = seconds(60).ns;  // a bound, not the expected end
  o.journal_path = fresh(name + ".fmj");
  o.status_path = fresh(name + ".status");
  o.program = "single-thread-test";
  SessionResult r;
  std::thread watcher([&] {
    r.traded = wait_until(
        [&] {
          const sim::server::SimServerStats s = fx.server.stats();
          return s.fills >= fills && s.orders_accepted >= 2 * fills;
        },
        40000);
    r.threads = thread_names();
    ::kill(::getpid(), SIGTERM);  // run_live's handler: the normal shutdown
  });
  r.rc = live::run_live(cfg, o);
  watcher.join();
  return r;
}

void check_replays(const std::string& path) {
  const bt::JournalInfo info = bt::inspect_journal(path);
  CHECK(info.has_session);
  Config::LoadOptions lo;
  lo.substitute_env = false;
  CHECK(Config::parse(info.config_toml, lo, "journal").single_threaded());
  REQUIRE(info.outbound_messages > 0);
  const bt::ReplayResult r = bt::replay_journal(path);
  MESSAGE("replay: events=" << r.events << " recorded=" << r.recorded_messages << " replayed="
                            << r.outbound_messages << " first_mismatch=" << r.first_mismatch);
  INFO("expected: " << r.expected_message);
  INFO("actual:   " << r.actual_message);
  CHECK(r.session_restored);
  CHECK(r.first_mismatch == -1);
  CHECK(r.outbound_messages == r.recorded_messages);
  CHECK(r.outbound_sha256 == r.recorded_sha256);
  CHECK(r.ok());
}

void check_status(const std::string& path) {
  StatusReader reader;
  std::string err;
  StatusSnapshot st;
  REQUIRE(reader.open(path, &err));
  REQUIRE(reader.read(st));
  CHECK(st.state == StatusRunState::Stopped);
  CHECK(st.orders_sent > 0);
  CHECK(st.fills > 0);
  CHECK(st.venues[0].orders_sent > 0);
  // T4 -> T5 is the venue's encode and write on the engine thread.
  CHECK(st.latency[static_cast<std::size_t>(LatencyInterval::Send)].count > 0);
}

}  // namespace

TEST_CASE("single thread: BasicMM trades over WebSocket on one thread and its journal replays") {
  ServerFixture fx;
  const SessionResult r = run_single(fx, false, "single-ws", 3);
  REQUIRE(r.traded);
  CHECK(r.rc == live::kExitOk);
  CHECK(r.threads.contains("fm-engine"));
  CHECK_FALSE(r.threads.contains("fm-net-0"));
  CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
  check_status((fastmm::test::tmp_dir() / "single-ws.status").string());
  check_replays((fastmm::test::tmp_dir() / "single-ws.fmj").string());
}

TEST_CASE("single thread: BasicMM trades over WebSocket + TLS and its journal replays") {
  ServerFixture fx(test_server_config(true));
  const SessionResult r = run_single(fx, true, "single-tls", 2);
  REQUIRE(r.traded);
  CHECK(r.rc == live::kExitOk);
  CHECK_FALSE(r.threads.contains("fm-net-0"));
  CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
  check_replays((fastmm::test::tmp_dir() / "single-tls.fmj").string());
}
