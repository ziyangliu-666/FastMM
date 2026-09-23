// Kill switch behaviour of whole fastmm-live sessions against the in-process simulator.
//   * fastmm::live::run_live with [engine] on_kill = "exit": a kill the engine trips itself
//     ([risk] max_loss, every fill loses money to a commission above the quoted spread) shuts the
//     session down with exit code 6 after cancel_all, leaves no open order, publishes the reason in
//     the final status, and its journal replays exactly;
//   * on_kill = "stay": the session keeps running with the kill switch engaged until it is stopped;
//   * a venue whose requests the exchange refuses (bad signature -> error map Fatal) trips that
//     venue's kill switch through the journaled TripVenueKill command; with one venue that is a
//     global kill, and the journal replays exactly.
#include "integration_util.hpp"

#include "fastmm/backtest/replay.hpp"
#include "fastmm/core/status_segment.hpp"
#include "fastmm/live/session.hpp"
#include "fastmm/strategies/builtin.hpp"
#include "fastmm/strategies/registry.hpp"

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
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

// A commission far above BasicMM's 5 bps half spread: net PnL is negative after every fill.
sim::server::SimServerConfig losing_fees_server() {
  sim::server::SimServerConfig c = test_server_config();
  c.maker_bps = 50.0;
  c.taker_bps = 50.0;
  return c;
}

Config kill_config(const ServerFixture& fx, const std::string& name, const char* on_kill) {
  Config cfg = sim_local_config(fx, false);
  cfg.engine.name = name;
  cfg.engine.on_kill = on_kill;
  cfg.engine.epoch_file = fresh(name + ".epoch");
  // A max_loss trip latches here; each session starts from a cleared state.
  cfg.engine.kill_file = fresh(name + ".kill");
  cfg.risk.max_loss = "0.01";
  return cfg;
}

live::LiveOptions session_options(const std::string& name) {
  live::LiveOptions o;
  o.duration_ns = seconds(60).ns;  // a bound, not the expected end
  o.journal_path = fresh(name + ".fmj");
  o.status_path = fresh(name + ".status");
  o.program = "kill-switch-test";
  return o;
}

bool read_status(const std::string& path, StatusSnapshot& out) {
  StatusReader r;
  std::string err;
  return r.open(path, &err) && r.read(out);
}

void check_replays(const std::string& path) {
  const bt::JournalInfo info = bt::inspect_journal(path);
  CHECK(info.version == kJournalVersion);
  CHECK(info.has_session);
  const bt::ReplayResult r = bt::replay_journal(path);
  MESSAGE("replay: events=" << r.events << " recorded=" << r.recorded_messages << " replayed="
                            << r.outbound_messages << " first_mismatch=" << r.first_mismatch);
  INFO("expected: " << r.expected_message);
  INFO("actual:   " << r.actual_message);
  CHECK(r.session_restored);
  CHECK(r.first_mismatch == -1);
  CHECK(r.outbound_messages == r.recorded_messages);
  CHECK(r.ok());
}

}  // namespace

TEST_CASE("sim_exchange kill switch: a max_loss kill ends the session with exit code 6") {
  register_strategies_once();
  ServerFixture fx(losing_fees_server());
  const Config cfg = kill_config(fx, "kill-exit", "exit");
  const live::LiveOptions o = session_options("kill-exit");
  const auto t0 = std::chrono::steady_clock::now();
  const int rc = live::run_live(cfg, o);
  const double took_s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  MESSAGE("run_live returned " << rc << " after " << took_s << " s");
  // 6, not 5: cancel_all succeeded on the venue; not 0: the session did not run to --duration.
  CHECK(rc == live::kExitKilled);
  CHECK(took_s < 50.0);
  CHECK(fx.server.stats().fills >= 1);
  CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));

  StatusSnapshot st;
  REQUIRE(read_status(o.status_path, st));
  CHECK(st.state == StatusRunState::Stopped);
  CHECK((st.kill_flags & 1U) != 0);
  CHECK(st.kill_reason == static_cast<std::uint8_t>(KillReason::MaxLoss));
  CHECK(st.kills == 1);  // the shutdown did not trip it a second time
  CHECK(format_status(st, st.updated_ns, false).find("stopped  KILLED (MaxLoss)") !=
        std::string::npos);
  // The trip follows from journaled inputs (fills, books, the risk limits in the embedded config).
  check_replays(o.journal_path);
}

TEST_CASE("sim_exchange kill switch: with on_kill = stay the session keeps running until stopped") {
  register_strategies_once();
  ServerFixture fx(losing_fees_server());
  const Config cfg = kill_config(fx, "kill-stay", "stay");
  const live::LiveOptions o = session_options("kill-stay");
  bool killed_while_running = false;
  bool still_running = false;
  std::thread watcher([&] {
    StatusSnapshot s;
    const bool killed = wait_until(
        [&] { return read_status(o.status_path, s) && (s.kill_flags & 1U) != 0; }, 45000);
    killed_while_running = killed && s.state == StatusRunState::Running;
    if (killed) {
      // Far longer than the control loop's 50 ms check plus a 250 ms status publication: a
      // session that exits on kill would be stopping or stopped by now.
      std::this_thread::sleep_for(std::chrono::milliseconds(1500));
      still_running = read_status(o.status_path, s) && s.state == StatusRunState::Running &&
                      (s.kill_flags & 1U) != 0;
    }
    ::kill(::getpid(), SIGTERM);  // run_live's handler: the normal shutdown
  });
  const int rc = live::run_live(cfg, o);
  watcher.join();
  CHECK(killed_while_running);
  CHECK(still_running);
  CHECK(rc == live::kExitOk);
  StatusSnapshot st;
  REQUIRE(read_status(o.status_path, st));
  CHECK(st.state == StatusRunState::Stopped);
  CHECK(st.kill_reason == static_cast<std::uint8_t>(KillReason::MaxLoss));
  CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
}

TEST_CASE("sim_exchange kill switch: a venue refusing the signature kills that venue and replays") {
  const std::string path = fresh("venue_kill.fmj");
  ServerFixture fx;
  Config cfg = sim_local_config(fx, false);
  cfg.venues[0].api_secret = "not-the-secret";
  LiveEngineOptions opts;
  opts.journal_path = path;
  LiveEngine live(cfg, opts);
  live.start();
  const bool killed =
      wait_until([&] { return (live.engine().live_stats().kill_flags & 1U) != 0; }, 30000);
  live.stop();
  REQUIRE(killed);
  const auto& e = live.engine();
  CHECK(e.risk().venue_killed(VenueId{0}));
  CHECK(e.venue_kill_reason(VenueId{0}) == KillReason::VenueFatal);
  CHECK(e.kill_reason() == KillReason::AllVenuesKilled);  // the only venue
  CHECK(e.stats().venue_kills == 1);
  CHECK(fx.server.stats().orders_accepted == 0);
  check_replays(path);
}
