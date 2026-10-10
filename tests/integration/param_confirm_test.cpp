// A parameter change confirmed end to end: fastmm-ctl param --wait returns once the engine of a
// real fastmm-live process has applied the update, `params` shows the values and the sequence
// numbers, and fastmm-top --json the source of the last update. fastmm-live and the tools are child
// processes against the in-process simulator.
#include "process_util.hpp"

#include "fastmm/store/sqlite_store.hpp"

#include <spawn.h>
#include <unistd.h>

#include <csignal>
#include <filesystem>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

#if defined(FASTMM_LIVE_EXE) && defined(FASTMM_TOP_EXE) && defined(FASTMM_CTL_EXE)

namespace {

// A program run to completion: its exit code and what it printed on stdout.
struct Output {
  int rc = -1;
  std::string out;
};
Output run(const char* exe, const std::vector<std::string>& args) {
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&fa, fds[0]);
  std::vector<const char*> argv{exe};
  for (const std::string& a : args) argv.push_back(a.c_str());
  argv.push_back(nullptr);
  pid_t pid = 0;
  const int rc =
      posix_spawn(&pid, exe, &fa, nullptr, const_cast<char* const*>(argv.data()), environ);
  posix_spawn_file_actions_destroy(&fa);
  ::close(fds[1]);
  REQUIRE_MESSAGE(rc == 0, "posix_spawn " << exe << " failed");
  Output o;
  char buf[4096];
  for (ssize_t n; (n = ::read(fds[0], buf, sizeof buf)) > 0;)
    o.out.append(buf, static_cast<std::size_t>(n));
  ::close(fds[0]);
  o.rc = reap(pid);
  return o;
}

}  // namespace

TEST_CASE("integration.params: fastmm-ctl param --wait returns once the engine has applied it") {
  ServerFixture fx;
  const SessionFiles f = write_config(fx, "param-confirm", "exit", "1000");
  // Short: sockaddr_un holds 107 bytes, and the build directory alone can be longer.
  const std::string ctl = (std::filesystem::temp_directory_path() /
                           ("fastmm-" + std::to_string(::getpid()) + "-param.ctl"))
                              .string();
  remove_all_of({f.epoch, f.kill, f.journal_dir, f.status, ctl});
  const pid_t live = spawn_live(f, 120, {"--control", ctl});
  const auto ctl_run = [&](std::vector<std::string> command) {
    command.insert(command.begin(), {"--path", ctl});
    return run(FASTMM_CTL_EXE, command);
  };
  REQUIRE_MESSAGE(wait_until([&] { return ctl_run({"params"}).rc == 0; }, 60000),
                  "the session never answered on its control socket");

  // Nothing published yet: the values are the configured ones (sim-local.toml).
  Output params = ctl_run({"params"});
  CHECK(params.out.starts_with("ok published=0 applied=0 pending=0 "));
  CHECK(params.out.find("\nhalf_spread_bps=5\n") != std::string::npos);

  const Output set =
      ctl_run({"--wait", "10000", "param", "half_spread_bps=7", "--source", "scheduled:drain"});
  CHECK(set.rc == 0);
  CHECK(set.out.find("ok param queued seq=1 source=scheduled:drain") != std::string::npos);
  CHECK(set.out.find("ok param applied seq=1\n") != std::string::npos);

  params = ctl_run({"params"});
  CHECK(params.out.starts_with("ok published=1 applied=1 pending=0 "));
  CHECK(params.out.find("last_origin=control last_source=scheduled:drain") != std::string::npos);
  CHECK(params.out.find("\nvalues=applied\n") != std::string::npos);
  CHECK(params.out.find("\nhalf_spread_bps=7\n") != std::string::npos);

  // A refused update is never queued: nothing to wait for, and the sequence does not move.
  const Output bad = ctl_run({"--wait", "1000", "param", "no_such_param=1"});
  CHECK(bad.rc == 1);
  CHECK(ctl_run({"params"}).out.starts_with("ok published=1 applied=1 "));

  // The status segment carries the last update and the control socket's sequence numbers.
  REQUIRE(wait_until(
      [&] {
        return run(FASTMM_TOP_EXE, {"--path", f.status, "--json"})
                   .out.find(R"("control_applied": 1)") != std::string::npos;
      },
      10000));
  const std::string json = run(FASTMM_TOP_EXE, {"--path", f.status, "--json"}).out;
  CHECK(json.find(R"("last_origin": "control", "last_source": "scheduled:drain", )"
                  R"("control_published": 1, "control_applied": 1, "pending": false)") !=
        std::string::npos);

  REQUIRE(::kill(live, SIGTERM) == 0);
  CHECK(reap(live) == 0);
  remove_all_of({ctl});

  // The store kept the history: every parameter the session started with, then the update with
  // its source.
  GenericSection section;
  section.values["path"] = f.journal_dir + "/param-confirm.db";
  store::BackendOptions o;
  o.config = &section;
  o.read_only = true;
  auto reader = store::make_sqlite_reader();
  REQUIRE(reader->open(o));
  store::QueryFilter all;
  auto changes = reader->param_changes(all);
  REQUIRE(changes);
  std::size_t initial = 0;
  bool drained = false;
  for (const auto& row : changes->rows) {
    if (row[3] == "initial") ++initial;
    if (row[3] == "control" && row[4] == "scheduled:drain" && row[6] == "half_spread_bps" &&
        row[7] == "7")
      drained = true;
  }
  CHECK(initial >= 8);  // basic_mm's whole parameter set, defaults included
  CHECK(drained);
  store::ParamQuery now;
  auto values = reader->params(now);
  REQUIRE(values);
  bool spread = false;
  for (const auto& row : values->rows) {
    if (row[0] == "half_spread_bps") spread = row[2] == "7" && row[5] == "scheduled:drain";
  }
  CHECK(spread);
}

#endif
