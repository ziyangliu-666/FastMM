#pragma once
// Whole programs against the in-process simulator: configs/sim-local.toml rewritten for the
// fixture, child processes (fastmm-live, fastmm-gateway) and their exit codes, and the position the
// engine's store recorded. The simulator stays in the test process; the programs are real children,
// because a crash has to be a crash.
#include "integration_util.hpp"

#include "fastmm/net/crypto.hpp"
#include "fastmm/store/reader.hpp"
#include "fastmm/store/registry.hpp"
#include "fastmm/venues/blocking_http.hpp"

#include <spawn.h>
#include <sys/wait.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

extern char** environ;

namespace fastmm::integration {

inline std::string tmp_path(const std::string& name) {
  return (fastmm::test::tmp_dir() / name).string();
}

inline void remove_all_of(const std::vector<std::string>& paths) {
  std::error_code ec;
  for (const std::string& p : paths) std::filesystem::remove_all(p, ec);
}

// configs/sim-local.toml aimed at the in-process simulator, with the session state under the
// test's tmp directory and the credentials written out literally.
struct SessionFiles {
  std::string config;
  std::string epoch;
  std::string kill;
  std::string journal_dir;
  std::string status;
};

inline SessionFiles write_config(const ServerFixture& fx,
                                 const std::string& stem,
                                 const std::string& on_kill,
                                 const std::string& max_loss,
                                 const std::string& spin_mode = "adaptive") {
  SessionFiles f;
  f.config = tmp_path(stem + ".toml");
  f.epoch = tmp_path(stem + ".epoch");
  f.kill = tmp_path(stem + ".kill");
  f.journal_dir = tmp_path(stem + "-runs");
  f.status = tmp_path(stem + ".status");

  std::string text = fastmm::test::read_file(repo_root() / "configs" / "sim-local.toml");
  auto replace_all = [&text](std::string_view from, const std::string& to) {
    for (std::size_t p = text.find(from); p != std::string::npos;
         p = text.find(from, p + to.size()))
      text.replace(p, from.size(), to);
  };
  replace_all("127.0.0.1:9080", "127.0.0.1:" + std::to_string(fx.server.port()));
  replace_all(R"(api_key = "${FASTMM_SIM_API_KEY}")",
              std::string(R"(api_key = ")") + kApiKey + "\"");
  replace_all(R"(api_secret = "${FASTMM_SIM_API_SECRET}")",
              std::string(R"(api_secret = ")") + kApiSecret + "\"");
  replace_all(R"(name = "sim-local")", "name = \"" + stem + "\"");
  replace_all(R"(journal_dir = "runs")", "journal_dir = \"" + f.journal_dir + "\"");
  replace_all(R"(epoch_file = "runs/session_epoch")", "epoch_file = \"" + f.epoch + "\"");
  replace_all(R"(max_loss = "50")", "max_loss = \"" + max_loss + "\"");
  replace_all(R"(spin_mode = "adaptive")", "spin_mode = \"" + spin_mode + "\"");
  replace_all("[engine]\n",
              "[engine]\nkill_file = \"" + f.kill + "\"\non_kill = \"" + on_kill + "\"\n");
  std::ofstream out(f.config, std::ios::trunc);
  REQUIRE(out.good());
  out << text;
  out.close();
  return f;
}

// Starts `exe` with `args` (argv[0] is added).
inline pid_t spawn_process(const char* exe, const std::vector<std::string>& args) {
  std::vector<const char*> argv{exe};
  for (const std::string& a : args) argv.push_back(a.c_str());
  argv.push_back(nullptr);
  pid_t pid = 0;
  const int rc =
      posix_spawn(&pid, exe, nullptr, nullptr, const_cast<char* const*>(argv.data()), environ);
  REQUIRE_MESSAGE(rc == 0, "posix_spawn " << exe << " failed");
  return pid;
}

#ifdef FASTMM_LIVE_EXE
// A child fastmm-live with its log next to the session's other files (read it when a case fails).
inline pid_t spawn_live(const SessionFiles& f,
                        int duration_s,
                        const std::vector<std::string>& extra = {}) {
  std::vector<std::string> args{"--config",
                                f.config,
                                "--duration",
                                std::to_string(duration_s) + "s",
                                "--status",
                                f.status,
                                "--log",
                                f.config + ".log"};
  args.insert(args.end(), extra.begin(), extra.end());
  return spawn_process(FASTMM_LIVE_EXE, args);
}
#endif

// Waits for the child and returns its exit code (-1 when it was signalled).
inline int reap(pid_t pid) {
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// The position of `symbol` the engine `engine`'s store recorded last (sqlite under journal_dir).
inline Qty store_position(const SessionFiles& f,
                          const std::string& engine,
                          std::string_view symbol = "BTCUSDT") {
  store::register_builtin_backends();
  auto reader = store::StoreRegistry::instance().make_reader("sqlite");
  REQUIRE(reader != nullptr);
  store::BackendOptions opts;
  opts.engine_name = engine;
  opts.default_dir = f.journal_dir;
  opts.read_only = true;
  REQUIRE(reader->open(opts).has_value());
  store::QueryFilter qf;
  qf.engine = engine;
  auto rec = reader->recovery(qf);
  REQUIRE(rec.has_value());
  REQUIRE(rec->found);
  Qty q{};
  for (const store::Recovery::PositionState& p : rec->position_state) {
    if (p.symbol == symbol) q = Qty::from_raw(p.qty_raw);
  }
  return q;
}

// A market order on the simulator's account over signed REST: a trade FastMM did not make. A
// running session gets it on its user stream as an execution of no order of its own; made while
// nothing runs, only the next session's execution replay can book it.
inline void outside_trade(const ServerFixture& fx,
                          std::string_view side,
                          std::string_view qty = "0.001") {
  const std::string query =
      "symbol=BTCUSDT&side=" + std::string(side) + "&type=MARKET&quantity=" + std::string(qty) +
      "&recvWindow=5000&timestamp=" + std::to_string(fx.server.server_time_ms());
  venues::BlockingHttp http(fx.http());
  const venues::HttpReply r =
      http.request("POST",
                   "/api/v3/order?" + query +
                       "&signature=" + std::string(net::hmac_sha256_hex(kApiSecret, query).view()),
                   std::string("X-MBX-APIKEY: ") + kApiKey + "\r\n");
  REQUIRE_MESSAGE(r.status == 200, r.body);
}

// The venue execution ids the store of `engine` holds in more than one session: each is an
// execution a restart booked a second time.
inline std::vector<std::string> booked_twice(const SessionFiles& f, const std::string& engine) {
  store::register_builtin_backends();
  auto reader = store::StoreRegistry::instance().make_reader("sqlite");
  REQUIRE(reader != nullptr);
  store::BackendOptions opts;
  opts.engine_name = engine;
  opts.default_dir = f.journal_dir;
  opts.read_only = true;
  REQUIRE(reader->open(opts).has_value());
  store::QueryFilter qf;
  qf.engine = engine;
  auto rows = reader->fills(qf);
  REQUIRE(rows.has_value());
  std::size_t exec = rows->columns.size();
  std::size_t session = rows->columns.size();
  for (std::size_t i = 0; i < rows->columns.size(); ++i) {
    if (rows->columns[i] == "exec_id") exec = i;
    if (rows->columns[i] == "session_id") session = i;
  }
  REQUIRE(exec < rows->columns.size());
  REQUIRE(session < rows->columns.size());
  std::map<std::string, std::set<std::string>> sessions_of;
  for (const std::vector<std::string>& row : rows->rows) {
    if (!row[exec].empty()) sessions_of[row[exec]].insert(row[session]);
  }
  std::vector<std::string> twice;
  for (const auto& [id, sessions] : sessions_of) {
    if (sessions.size() > 1) twice.push_back(id);
  }
  return twice;
}

// The number of fills the store of `engine` holds (every session).
inline std::size_t stored_fills(const SessionFiles& f, const std::string& engine) {
  store::register_builtin_backends();
  auto reader = store::StoreRegistry::instance().make_reader("sqlite");
  REQUIRE(reader != nullptr);
  store::BackendOptions opts;
  opts.engine_name = engine;
  opts.default_dir = f.journal_dir;
  opts.read_only = true;
  REQUIRE(reader->open(opts).has_value());
  store::QueryFilter qf;
  qf.engine = engine;
  auto rows = reader->fills(qf);
  REQUIRE(rows.has_value());
  return rows->rows.size();
}

}  // namespace fastmm::integration
