// A real fastmm-live against the simulator, with a real fastmm-shadow following its journal:
// through rotations, a clean restart and a crash. For each session and point, the shadow's outbound
// hash must be the hash of a backtest of the finished journal with the same configuration, and the
// shadow's live side must be what the session's own journal says.
#include "fastmm/backtest/shadow.hpp"

#include "process_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/registrations.hpp"
#include "fastmm/backtest/sweep.hpp"
#include "fastmm/core/shadow_status.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

namespace {

#if defined(FASTMM_LIVE_EXE) && defined(FASTMM_SHADOW_EXE) && defined(FASTMM_CTL_EXE)

constexpr int kWindowS = 2;
constexpr int kSettleS = 5;

std::string read_all(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

// The session's configuration, with a shadow of it and parts small enough to rotate every few
// seconds.
SessionFiles shadow_config_file(const ServerFixture& fx, const std::string& stem, bool enabled) {
  SessionFiles f = write_config(fx, stem, "exit", "10000");
  // Nothing of an earlier run: the shadow would start from its newest session.
  remove_all_of({f.journal_dir, f.epoch, f.kill, f.status, f.status + ".shadow"});
  std::string text = read_all(f.config);
  const std::size_t engine = text.find("[engine]\n");
  REQUIRE(engine != std::string::npos);
  text.insert(engine + 9, "journal_max_bytes = 1048576\n");
  text += fmt::format(R"(
[backtest]
fill_model = "l2_queue"
latency_fixed_us = 250

[shadow]
enabled = {}
window_s = {}
settle_s = {}
threads = 2
status = "{}.shadow"

[shadow.grid]
half_spread_bps = [3.0, 8.0]

[[shadow.points]]
quote_qty = 0.002
)",
                      enabled ? "true" : "false",
                      kWindowS,
                      kSettleS,
                      f.status);
  std::ofstream out(f.config, std::ios::trunc);
  out << text;
  return f;
}

struct ReportedPoint {
  std::string sha;
  std::uint64_t messages = 0;
};
struct ReportedSession {
  std::string journal;
  bool finished = false;
  bool writer_gone = false;
  std::uint64_t md_events = 0;
  std::uint64_t parts = 0;
  std::uint64_t late = 0;
  std::vector<ReportedPoint> points;
};

// The value after `"key": ` at or after `at`: up to the next comma or closing brace, quotes off.
std::string field(const std::string& text, std::string_view key, std::size_t& at) {
  const std::string name = fmt::format("\"{}\": ", key);
  const std::size_t p = text.find(name, at);
  if (p == std::string::npos) return {};
  std::size_t from = p + name.size();
  std::size_t to = text[from] == '"' ? text.find('"', ++from) : text.find_first_of(",}", from);
  at = to;
  return text.substr(from, to - from);
}

// The report's "sessions", as far as the test reads them.
std::vector<ReportedSession> sessions_of(const std::string& report) {
  std::vector<ReportedSession> out;
  const std::size_t start = report.find("\"sessions\":");
  if (start == std::string::npos) return out;
  const std::string text = report.substr(start);
  std::size_t at = 0;
  for (;;) {
    const std::size_t row = text.find("\"session_id\":", at);
    if (row == std::string::npos) return out;
    at = row;
    ReportedSession s;
    s.journal = field(text, "journal", at);
    s.finished = field(text, "finished", at) == "true";
    s.md_events = std::stoull(field(text, "md_events", at));
    s.parts = std::stoull(field(text, "parts", at));
    s.late = std::stoull(field(text, "late", at));
    s.writer_gone = field(text, "writer_gone", at) == "true";
    const std::size_t next = text.find("\"session_id\":", at);
    for (;;) {
      std::size_t p = at;
      const std::string sha = field(text, "outbound_sha256", p);
      if (sha.empty() || (next != std::string::npos && p > next)) break;
      at = p;
      s.points.push_back(ReportedPoint{sha, std::stoull(field(text, "outbound_messages", at))});
    }
    out.push_back(std::move(s));
  }
}

// Each point as a backtest of the finished journal.
void check_against_finished(const ReportedSession& s, const bt::SweepPoints& variants) {
  CAPTURE(s.journal);
  bt::JournalSource probe(s.journal, true);
  const bt::BacktestConfig start =
      bt::shared_start(bt::shadow_config(probe.reader(), *probe.recorded(), seconds(kWindowS)));
  std::vector<bt::BacktestConfig> cfgs{bt::point_config(start, ParamMap{})};
  for (const ParamMap& p : variants) cfgs.push_back(bt::point_config(start, p));
  REQUIRE(s.points.size() == cfgs.size());
  for (std::size_t i = 0; i < cfgs.size(); ++i) {
    CAPTURE(i);
    bt::JournalSource source(s.journal, true);
    const bt::BacktestResult r = bt::run_backtest(cfgs[i], cfgs[i].strategy, &source);
    CHECK(r.outbound_sha256 == s.points[i].sha);
    CHECK(r.outbound_messages == s.points[i].messages);
  }
}

#endif

}  // namespace

#if defined(FASTMM_LIVE_EXE) && defined(FASTMM_SHADOW_EXE) && defined(FASTMM_CTL_EXE)

TEST_CASE(
    "integration.shadow: a shadow follows live sessions through rotation, restart and crash") {
  ServerFixture fx;
  bt::register_builtin_strategies();
  const SessionFiles f = shadow_config_file(fx, "shadow-e2e", false);
  const std::string report = f.journal_dir + "/shadow-e2e.shadow.json";
  std::filesystem::create_directories(f.journal_dir);
  // The shadow is there first: it waits for a session.
  const pid_t shadow = spawn_process(FASTMM_SHADOW_EXE, {"--config", f.config});
  // A session that ends cleanly, rotates its journal on the way, and takes an operator's commands.
  const std::string control = fmt::format("/tmp/shadow-e2e-{}.ctl", ::getpid());
  const pid_t first = spawn_live(f, 28, {"--control", control});
  std::this_thread::sleep_for(std::chrono::seconds(8));
  const auto ctl = [&](const std::vector<std::string>& command) {
    std::vector<std::string> args{"--path", control};
    args.insert(args.end(), command.begin(), command.end());
    CHECK(reap(spawn_process(FASTMM_CTL_EXE, args)) == 0);
  };
  ctl({"param", "half_spread_bps=7"});
  std::this_thread::sleep_for(std::chrono::seconds(3));
  ctl({"pull"});
  std::this_thread::sleep_for(std::chrono::seconds(3));
  ctl({"resume"});
  ctl({"limits", "max_position=0.015"});
  CHECK(reap(first) == 0);
  std::filesystem::remove(control);
  // A restart: the next session, which the shadow finds by itself; killed.
  const pid_t second = spawn_live(f, 60);
  std::this_thread::sleep_for(std::chrono::seconds(7));
  REQUIRE(::kill(second, SIGKILL) == 0);
  CHECK(reap(second) == -1);
  // And a third after the crash.
  const pid_t third = spawn_live(f, 8);
  CHECK(reap(third) == 0);
  // Done when the shadow has finished all three. The crashed one's writer is taken to be gone once
  // a newer session exists.
  std::vector<ReportedSession> sessions;
  const bool all_done = wait_until(
      [&] {
        sessions = sessions_of(read_all(report));
        return sessions.size() == 3 && sessions[2].finished;
      },
      60000);
  INFO(read_all(report));
  REQUIRE(all_done);
  // The status file and fastmm-top's reader.
  ShadowStatusReader status;
  REQUIRE(status.open(f.status + ".shadow"));
  ShadowStatus st;
  REQUIRE(status.read(st));
  CHECK(st.sessions == 3);
  CHECK(st.windows >= 3);
  CHECK(st.points == 4);
  CHECK(st.live.fills > 0);
  CHECK(st.sim.fills > 0);
  CHECK(st.variants == 3);
  CHECK(format_shadow_status(st, wall_now().ns).find("half_spread_bps=3") != std::string::npos);
  CHECK(shadow_status_prometheus(st, wall_now().ns).find("fastmm_shadow_net_error") !=
        std::string::npos);
  CHECK(::kill(shadow, SIGTERM) == 0);
  CHECK(reap(shadow) == 0);
  // Exact: each session and each point against a backtest of its finished journal.
  const bt::SweepPoints variants = {
      {{"half_spread_bps", "3.0"}}, {{"half_spread_bps", "8.0"}}, {{"quote_qty", "0.002"}}};
  CHECK(sessions[0].parts >= 2);  // it rotated
  CHECK_FALSE(sessions[0].writer_gone);
  CHECK(sessions[1].writer_gone);  // the crash: no trailer, and a newer session came
  CHECK_FALSE(sessions[2].writer_gone);
  for (const ReportedSession& s : sessions) {
    MESSAGE(s.journal << ": " << s.md_events << " events, " << s.parts << " parts, " << s.late
                      << " late");
    CHECK(s.late == 0);
    check_against_finished(s, variants);
  }
}

TEST_CASE("integration.shadow: a session does not notice its shadow dying") {
  ServerFixture fx;
  const SessionFiles f = shadow_config_file(fx, "shadow-dies", true);
  const pid_t live = spawn_live(f, 20);
  ShadowStatusReader status;
  ShadowStatus st;
  REQUIRE(wait_until([&] { return status.open(f.status + ".shadow") && status.read(st); }, 15000));
  REQUIRE(st.pid > 0);
  // It stops answering first, as a hung one would, then dies.
  CHECK(::kill(static_cast<pid_t>(st.pid), SIGSTOP) == 0);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  CHECK(::kill(static_cast<pid_t>(st.pid), SIGKILL) == 0);
  CHECK(reap(live) == 0);
  // The session's own record is whole.
  const std::vector<std::filesystem::path> journals = [&] {
    std::vector<std::filesystem::path> out;
    for (const auto& e : std::filesystem::directory_iterator(f.journal_dir)) {
      if (e.path().extension() == ".fmj") out.push_back(e.path());
    }
    return out;
  }();
  REQUIRE(journals.size() == 1);
  JournalReader r;
  REQUIRE(r.open(journals[0].string()));
  CHECK(r.complete());
}

TEST_CASE("integration.shadow: [shadow] enabled starts it with the session") {
  ServerFixture fx;
  const SessionFiles f = shadow_config_file(fx, "shadow-on", true);
  const std::string report = f.journal_dir + "/shadow-on.shadow.json";
  const pid_t live = spawn_live(f, 8);
  CHECK(reap(live) == 0);
  // The live session is over; the shadow it started finishes the journal and reports.
  std::vector<ReportedSession> sessions;
  const bool done = wait_until(
      [&] {
        sessions = sessions_of(read_all(report));
        return sessions.size() == 1 && sessions[0].finished;
      },
      30000);
  INFO(read_all(report));
  CHECK(done);
  // It left when asked to: nothing keeps running after the test (the shadow waits for a next
  // session for [shadow] idle_exit_s).
  ShadowStatusReader status;
  REQUIRE(status.open(f.status + ".shadow"));
  ShadowStatus st;
  REQUIRE(status.read(st));
  const auto pid = static_cast<pid_t>(st.pid);
  CHECK(pid > 0);
  if (pid > 0) ::kill(pid, SIGTERM);
}

#endif
