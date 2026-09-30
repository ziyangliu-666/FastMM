// xmm with a fallback hedge venue through a real crash: fastmm-live is a child process quoting on
// one in-process simulator (A), hedging on a second (B) and falling back to a third (C). B refuses
// every order, so after max_hedge_failures the hedge goes to C; the process is killed with SIGKILL
// while C holds that hedge's reply. The simulators outlive it, so the venue-side facts decide: the
// venues accepted exactly one hedge per maker fill and net to zero, before and after the restart,
// and the restarted process fails over to C again for the next fill.
#include "gateway_util.hpp"
#include "process_util.hpp"

#include "fastmm/live/session.hpp"

#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

#ifdef FASTMM_LIVE_EXE

namespace {

// No market orders: our quotes trade only when the test fills them.
sim::server::SimServerConfig quiet(std::uint64_t seed) {
  sim::server::SimServerConfig c = test_server_config();
  c.seed = seed;
  c.generator.market_rate_per_s = 0.0;
  return c;
}

std::string venue_toml(const char* name, const ServerFixture& fx) {
  std::ostringstream o;
  o << "[venues." << name << "]\nkind = \"binance_spot\"\n"
    << "ws_url = \"" << fx.ws() << "/stream\"\n"
    << "ws_api_url = \"" << fx.ws() << "/ws-api/v3\"\n"
    << "rest_url = \"" << fx.http() << "\"\n"
    << "api_key = \"" << kApiKey << "\"\napi_secret = \"" << kApiSecret << "\"\n"
    << "testnet = true\nsupports_replace = false\nrecv_window_ms = 3000\n"
    << "[venues." << name << ".fees]\nmaker_bps = 1.0\ntaker_bps = 4.0\n";
  return o.str();
}

std::string instrument_toml(const char* venue) {
  return std::string("[[instruments]]\nvenue = \"") + venue +
         "\"\nsymbol = \"BTCUSDT\"\nbase = \"BTC\"\nquote = \"USDT\"\nasset_class = \"spot\"\n"
         "tick = \"0.01\"\nlot = \"0.00001\"\nmin_qty = \"0.00001\"\nmax_qty = \"100\"\n"
         "min_notional = \"5\"\nenabled = true\n";
}

// A fresh stem per run: the pid and the time, so no earlier run's files are in the way.
SessionFiles session_files(const std::string& stem) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::string s = stem + "-" + std::to_string(::getpid()) + "-" + std::to_string(now);
  SessionFiles f;
  f.config = tmp_path(s + ".toml");
  f.epoch = tmp_path(s + ".epoch");
  f.kill = tmp_path(s + ".kill");
  f.journal_dir = tmp_path(s + "-runs");
  f.status = tmp_path(s + ".status");
  return f;
}

std::string engine_name(const SessionFiles& f) {
  return std::filesystem::path(f.config).stem().string();
}

// xmm quoting instrument 0 on A, hedging on instrument 1 (B), falling back to instrument 2 (C);
// the store on, positions restored at start.
void write_config(const SessionFiles& f,
                  const ServerFixture& a,
                  const ServerFixture& b,
                  const ServerFixture& c) {
  std::string text = "[engine]\nname = \"" + engine_name(f) + "\"\n" + R"(cpu = -1
net_cpus = []
spin_mode = "adaptive"
journal = true
rng_seed = 42
md_ring_bytes = 4194304
order_ring_bytes = 1048576
journal_ring_bytes = 16777216
max_events_per_step = 64
crossed_grace_ms = 100
latency_publish_ms = 1000
min_requote_ticks = 1
min_requote_interval_ms = 50
min_qty_bps = 8000
post_only = true
supports_replace = false
)";
  text += "journal_dir = \"" + f.journal_dir + "\"\n";
  text += "epoch_file = \"" + f.epoch + "\"\n";
  text += "kill_file = \"" + f.kill + "\"\n";
  text += venue_toml("a", a);
  text += venue_toml("b", b);
  text += venue_toml("c", c);
  text += instrument_toml("a");
  text += instrument_toml("b");
  text += instrument_toml("c");
  text += R"([strategy]
name = "xmm"

[strategy.params]
quote_instrument = 0
hedge_instrument = 1
fallback_instrument = 2
quote_qty = 0.001
edge_bps = 2
slippage_bps = 1
hedge_tolerance_bps = 20
fallback_tolerance_bps = 20
basis_halflife_s = 30
requote_threshold_ticks = 50
stale_ms = 5000
max_unhedged = 0.003
hedge_retry_ms = 100
max_hedge_failures = 2
uncertain_hold_ms = 300
failover_bench_ms = 600000

[risk]
max_order_qty = "0.01"
max_order_notional = "1000"
max_position = "0.02"
max_open_orders = 8
price_collar_bps = 100
fat_finger_bps = 500
stale_md_ms = 5000
max_loss = "50"
orders_per_sec = 20
burst = 10
stp = true

[logging]
level = "info"
file = ""
mirror_level = "warn"
)";
  std::ofstream out(f.config, std::ios::trunc);
  REQUIRE(out.good());
  out << text;
}

std::string log_of(const SessionFiles& f) {
  std::error_code ec;
  if (!std::filesystem::exists(f.config + ".log", ec)) return {};
  return fastmm::test::read_file(f.config + ".log");
}

std::vector<sim::server::SimExchangeServer::OpenOrder> resting_of(const ServerFixture& fx,
                                                                  std::uint16_t epoch) {
  std::vector<sim::server::SimExchangeServer::OpenOrder> out;
  for (const auto& o : fx.server.open_orders()) {
    const auto cl = decode_cl_ord_id(o.client_order_id);
    if (cl && cl_ord_id_epoch(*cl) == epoch) out.push_back(o);
  }
  return out;
}

std::uint16_t resting_epoch(const ServerFixture& fx, std::uint16_t other = 0) {
  for (const std::string& id : fx.server.open_client_order_ids()) {
    const auto cl = decode_cl_ord_id(id);
    if (cl && cl_ord_id_epoch(*cl) != other) return cl_ord_id_epoch(*cl);
  }
  return 0;
}

struct Venues {
  ServerFixture a{quiet(7)};
  ServerFixture b{quiet(11)};
  ServerFixture c{quiet(13)};
  [[nodiscard]] Qty net() const {
    return a.server.stats().position + b.server.stats().position + c.server.stats().position;
  }
  // Hedges the venues took: B's and C's accepted orders.
  [[nodiscard]] std::uint64_t hedges() const {
    return b.server.stats().orders_accepted + c.server.stats().orders_accepted;
  }
};

// A session quotes both sides on A and both hedge venues' private channels are up; its epoch.
std::uint16_t wait_quoting(const Venues& v, const SessionFiles& f, std::uint16_t previous = 0) {
  std::uint16_t epoch = 0;
  REQUIRE_MESSAGE(wait_until(
                      [&] {
                        epoch = resting_epoch(v.a, previous);
                        return epoch != 0 && resting_of(v.a, epoch).size() == 2 &&
                               v.b.server.stats().user_subscriptions >= 1 &&
                               v.c.server.stats().user_subscriptions >= 1;
                      },
                      45000),
                  "the session never quoted both sides: " << log_of(f));
  return epoch;
}

Qty fill_resting(ServerFixture& fx, std::uint16_t epoch) {
  for (const auto& o : resting_of(fx, epoch)) {
    return fx.server.fill_open_order(o.client_order_id, Qty{});
  }
  return Qty{};
}

struct Child {
  pid_t pid = -1;
  explicit Child(pid_t p) : pid(p) {}
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
  ~Child() {
    if (pid <= 0) return;
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
  }
};

}  // namespace

TEST_CASE("xmm failover: B refuses, C takes the hedge, SIGKILL mid-hedge and a restart") {
  Venues v;
  const SessionFiles f = session_files("xmm-failover");
  write_config(f, v.a, v.b, v.c);
  v.b.server.reject_next_orders(100000);  // B refuses every order, across both sessions

  Child first(spawn_live(f, 300));
  const std::uint16_t e1 = wait_quoting(v, f);
  v.c.server.set_ack_delay_ms(600'000);  // C carries the hedge out; the process hears nothing
  REQUIRE(fill_resting(v.a, e1).is_positive());
  // Two refusals on B (each an unreported outcome, held uncertain_hold_ms), then C.
  REQUIRE_MESSAGE(wait_until(
                      [&] {
                        return v.b.server.stats().orders_rejected >= 2 &&
                               v.c.server.stats().orders_accepted == 1 && v.net().is_zero();
                      },
                      15000),
                  "no failover to C: " << log_of(f));
  REQUIRE(::kill(first.pid, SIGKILL) == 0);
  CHECK(reap(first.pid) == -1);
  first.pid = -1;
  v.c.server.set_ack_delay_ms(0);
  CHECK(v.b.server.stats().orders_accepted == 0);
  CHECK(v.hedges() == 1);
  CHECK(v.net().is_zero());

  // The restarted process replays C's execution: flat, no hedge.
  Child second(spawn_live(f, 300));
  const std::uint16_t e2 = wait_quoting(v, f, e1);
  CHECK(e2 != e1);
  std::this_thread::sleep_for(std::chrono::seconds(3));  // 30 of xmm's timer periods
  INFO("second session: " << log_of(f));
  CHECK(v.hedges() == 1);
  CHECK(v.net().is_zero());

  // The next fill: B still refuses, and the new process fails over to C on its own.
  const std::uint64_t rejected = v.b.server.stats().orders_rejected;
  REQUIRE(fill_resting(v.a, e2).is_positive());
  CHECK(wait_until(
      [&] {
        return v.b.server.stats().orders_rejected >= rejected + 2 &&
               v.c.server.stats().orders_accepted == 2 && v.net().is_zero();
      },
      15000));
  std::this_thread::sleep_for(std::chrono::seconds(3));
  CHECK(v.b.server.stats().orders_accepted == 0);
  CHECK(v.hedges() == 2);  // one per maker fill
  CHECK(v.net().is_zero());
  // The log is written by the logger thread; wait for the line rather than read it once.
  CHECK(wait_until(
      [&] {
        return log_of(f).find(
                   "hedging moves from BTCUSDT (instrument 1) to BTCUSDT (instrument 2)") !=
               std::string::npos;
      },
      10000));

  REQUIRE(::kill(second.pid, SIGTERM) == 0);
  CHECK(reap(second.pid) == live::kExitOk);
  second.pid = -1;
  CHECK(booked_twice(f, engine_name(f)).empty());
  CHECK(v.a.server.stats().duplicate_client_order_ids == 0);
  CHECK(v.c.server.stats().duplicate_client_order_ids == 0);
}

#endif  // FASTMM_LIVE_EXE
