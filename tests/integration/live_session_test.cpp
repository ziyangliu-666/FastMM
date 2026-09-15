// fastmm::live::run_live with a LiveStrategy, the path fastmm_live._live takes for Python hot
// strategies, against the in-process simulator: a HotStrategy with a hook written in C, its
// parameter ring and journal metadata, a failing hook (exit code 6), the watchdog (exit code 7),
// the restored signal handlers, and confine_threads.
#include "integration_util.hpp"

#include "fastmm/backtest/replay.hpp"
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/live/live_backend.hpp"
#include "fastmm/live/session.hpp"
#include "fastmm/live/thread_affinity.hpp"
#include "fastmm/strategies/hot_abi.h"
#include "fastmm/strategies/hot_params.hpp"
#include "fastmm/strategies/hot_strategy.hpp"
#include "fastmm/strategies/param_publisher.hpp"

#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

namespace {

using HotLiveEngine = Engine<HotStrategy, TscClock, LiveTransport, RingFeed>;

std::atomic<std::int64_t> g_quotes{0};
std::atomic<bool> g_fail{false};

// One level per side at mid -/+ half_spread_bps (the double parameter at offset 0), 0.001 each.
std::int32_t quote_mid(std::uint8_t* self, fastmm_hot_ctx* c, fastmm_hot_book* b) {
  if (g_fail.load(std::memory_order_relaxed)) return FASTMM_HOT_EXCEPTION;
  if (b->valid == 0) return FASTMM_HOT_OK;
  double bps = 0.0;
  std::memcpy(&bps, self, sizeof bps);
  const double half = b->mid * bps * 1e-4;
  c->bid_px[0] = b->mid - half;
  c->bid_qty[0] = 0.001;
  c->ask_px[0] = b->mid + half;
  c->ask_qty[0] = 0.001;
  c->n_bids = 1;
  c->n_asks = 1;
  c->flags = FASTMM_HOT_FLAG_KEEP_PASSIVE;
  c->action = FASTMM_HOT_ACTION_QUOTE;
  g_quotes.fetch_add(1, std::memory_order_relaxed);
  return FASTMM_HOT_OK;
}

std::vector<HotParamField> param_fields() {
  std::vector<HotParamField> f(1);
  f[0].name = "half_spread_bps";
  f[0].type = ParamType::Double;
  f[0].offset = 0;
  f[0].raw_offset = 8;
  f[0].min = 0.0;
  f[0].max = 1000.0;
  return f;
}

// What fastmm_live._live builds: the parameter table, ring and publisher, the program and the
// LiveStrategy around them.
struct HotSession {
  HotParamTable table{param_fields(), 16};
  MsgRing ring{1U << 16};
  std::unique_ptr<ParamPublisher> publisher;
  HotProgram program;
  live::LiveStrategy strategy;
  HotStrategy* hot = nullptr;
  std::uint64_t param_updates = 0;
  bool failed = false;
  HotError error{};

  HotSession() {
    program.hooks[static_cast<std::size_t>(HotHook::Book)] = &quote_mid;
    program.record.assign(16, 0);
    const double bps = 5.0;
    const std::int64_t raw = 500'000'000;
    std::memcpy(program.record.data(), &bps, 8);
    std::memcpy(program.record.data() + 8, &raw, 8);
    program.param_bytes = 16;
    program.params = table.slots();
    publisher = table.publisher(ParamSink::to_ring(ring), program.record, 1);
    strategy.name = "hot_test";
    strategy.params = &table.schema();
    strategy.meta = "class=tests:HotSession\n";
    strategy.inputs.push_back(&ring);
    strategy.make = [this](RunnerDeps& deps) {
      std::unique_ptr<IEngineRunner> runner =
          live::make_live_runner<HotStrategy>(TransportKind::Live, deps);
      auto* er = static_cast<EngineRunner<HotLiveEngine, HotStrategy>*>(runner.get());
      REQUIRE(er->strategy().attach(program, er->engine().instruments()));
      hot = &er->strategy();
      return runner;
    };
    strategy.finished = [this](IEngineRunner& r) {
      auto& er = static_cast<EngineRunner<HotLiveEngine, HotStrategy>&>(r);
      param_updates = er.engine().stats().param_updates;
      failed = hot->failed();
      error = hot->error();
    };
  }
};

live::LiveOptions options(HotSession& s, std::int64_t duration_ns, const std::string& journal) {
  live::LiveOptions o;
  o.duration_ns = duration_ns;
  o.no_status = true;
  o.journal_path = journal;
  o.no_journal = journal.empty();
  o.strategy = &s.strategy;
  return o;
}

Config session_config(const ServerFixture& fx) {
  Config cfg = sim_local_config(fx, false);
  cfg.strategy.name = "hot_test";
  cfg.strategy.params.clear();
  cfg.engine.epoch_file = (fastmm::test::tmp_dir() / "live_session_epoch").string();
  return cfg;
}

bool quotes_at_least(std::int64_t n, int timeout_ms) {
  return wait_until([&] { return g_quotes.load() >= n; }, timeout_ms);
}

}  // namespace

TEST_CASE("live session: a LiveStrategy trades, applies a publish, journals its metadata") {
  g_quotes = 0;
  g_fail = false;
  const std::string journal = (fastmm::test::tmp_dir() / "live_session_hot.fmj").string();
  std::filesystem::remove(journal);
  ServerFixture fx;
  HotSession s;
  const Config cfg = session_config(fx);
  const live::LiveOptions opts = options(s, 5'000'000'000, journal);
  struct sigaction before {};
  sigaction(SIGINT, nullptr, &before);

  int rc = -1;
  std::thread session([&] { rc = live::run_live(cfg, opts); });
  const bool quoted = quotes_at_least(3, 10000);
  CHECK(s.publisher->publish({{"half_spread_bps", "6.5"}}));
  session.join();
  REQUIRE(quoted);
  CHECK(rc == live::kExitOk);
  CHECK(s.param_updates == 1);
  CHECK_FALSE(s.failed);
  CHECK(fx.server.stats().orders_accepted > 0);
  s.publisher->close();
  CHECK_FALSE(s.publisher->publish({{"half_spread_bps", "7"}}));

  struct sigaction after {};
  sigaction(SIGINT, nullptr, &after);
  CHECK(after.sa_handler == before.sa_handler);  // run_live restored the handler it replaced

  const bt::JournalInfo info = bt::inspect_journal(journal);
  CHECK(info.strategy == "hot_test");
  CHECK(info.strategy_meta == "class=tests:HotSession\n");
  CHECK(info.outbound_messages > 0);
  JournalReader reader;
  REQUIRE(reader.open(journal).has_value());
  REQUIRE(reader.params().size() == 1);
  CHECK(reader.params()[0].name == "half_spread_bps");
  CHECK(reader.params()[0].type == static_cast<std::uint8_t>(ParamType::Double));
  std::size_t updates = 0;
  reader.for_each([&](const EventHeader* e) {
    if (e->type == EventType::ParamUpdate) ++updates;
  });
  CHECK(updates == 1);
}

TEST_CASE("live session: a failing hot hook trips StrategyError and exits with code 6") {
  g_quotes = 0;
  g_fail = false;
  ServerFixture fx;
  HotSession s;
  const Config cfg = session_config(fx);
  REQUIRE(cfg.engine.on_kill != "stay");
  const live::LiveOptions opts = options(s, 30'000'000'000, "");
  int rc = -1;
  const auto start = std::chrono::steady_clock::now();
  std::thread session([&] { rc = live::run_live(cfg, opts); });
  const bool quoted = quotes_at_least(2, 10000);
  g_fail = true;
  session.join();
  g_fail = false;
  REQUIRE(quoted);
  CHECK(rc == live::kExitKilled);
  CHECK(s.failed);
  CHECK(s.error.status == FASTMM_HOT_EXCEPTION);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(25));
}

TEST_CASE("live session: a watchdog cause stops the session with exit code 7") {
  g_quotes = 0;
  g_fail = false;
  ServerFixture fx;
  HotSession s;
  const Config cfg = session_config(fx);
  live::LiveOptions opts = options(s, 30'000'000'000, "");
  std::atomic<bool> fail_slow{false};
  opts.watchdog = [&]() -> std::string {
    return fail_slow.load() ? "slow tier: test failure" : std::string();
  };
  int rc = -1;
  std::thread session([&] { rc = live::run_live(cfg, opts); });
  const bool quoted = quotes_at_least(2, 10000);
  fail_slow = true;
  session.join();
  REQUIRE(quoted);
  CHECK(rc == live::kExitSlowTier);
  CHECK_FALSE(s.failed);
}

TEST_CASE("live session: confine_threads moves every thread off the reserved CPUs") {
  cpu_set_t original;
  CPU_ZERO(&original);
  REQUIRE(sched_getaffinity(0, sizeof original, &original) == 0);
  if (CPU_COUNT(&original) < 2) {
    MESSAGE("skipped: fewer than 2 CPUs");
    return;
  }
  int first = 0;
  while (!CPU_ISSET(static_cast<std::size_t>(first), &original)) ++first;

  std::atomic<pid_t> helper_tid{0};
  std::atomic<bool> stop{false};
  std::thread helper([&] {
    helper_tid = static_cast<pid_t>(::syscall(SYS_gettid));
    while (!stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  });
  REQUIRE(wait_until([&] { return helper_tid.load() != 0; }, 1000));

  CHECK(live::confine_threads(std::vector<int>{-1}).cpus.empty());  // nothing reserved
  const live::ConfineResult r = live::confine_threads(std::vector<int>{first});
  CHECK(r.error.empty());
  CHECK(r.threads >= 2);
  CHECK(std::find(r.cpus.begin(), r.cpus.end(), first) == r.cpus.end());
  cpu_set_t mask;
  CPU_ZERO(&mask);
  REQUIRE(sched_getaffinity(helper_tid.load(), sizeof mask, &mask) == 0);
  CHECK_FALSE(CPU_ISSET(static_cast<std::size_t>(first), &mask));
  CHECK(CPU_COUNT(&mask) == CPU_COUNT(&original) - 1);

  // Every CPU of the (confined) process reserved: nothing changes.
  std::vector<int> all;
  for (int c = 0; c < CPU_SETSIZE; ++c) {
    if (CPU_ISSET(static_cast<std::size_t>(c), &original)) all.push_back(c);
  }
  CHECK_FALSE(live::confine_threads(all).error.empty());

  stop = true;
  helper.join();
  for (const auto& e : std::filesystem::directory_iterator("/proc/self/task")) {
    const pid_t tid = std::stoi(e.path().filename().string());
    static_cast<void>(sched_setaffinity(tid, sizeof original, &original));
  }
}
