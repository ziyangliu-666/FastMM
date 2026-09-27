// xmm through a real crash: fastmm-live is a child process quoting on one in-process simulator and
// hedging on another, and it is killed with SIGKILL in the middle of a hedge. The simulators
// outlive it, so the venue-side facts decide: no lost fill (the restarted process books every
// execution, its store ends equal to each venue's position), no duplicated hedge (the hedge venue
// accepted exactly one order per maker fill, the venues net to zero) and no bypassed risk limit
// (`[risk.underlying.BTC] max_net` holds on the venues while the restarted process re-hedges).
#include "gateway_util.hpp"
#include "process_util.hpp"

#include "fastmm/live/session.hpp"

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

#ifdef FASTMM_LIVE_EXE

namespace {

// No market orders: our quotes trade only when the test fills them. The limit flow keeps both
// books moving and gives the hedge liquidity to take.
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

// One process's files; the stem carries the pid so parallel runs of this binary do not collide.
SessionFiles session_files(const std::string& stem) {
  const std::string s = stem + "-" + std::to_string(::getpid());
  SessionFiles f;
  f.config = tmp_path(s + ".toml");
  f.epoch = tmp_path(s + ".epoch");
  f.kill = tmp_path(s + ".kill");
  f.journal_dir = tmp_path(s + "-runs");
  f.status = tmp_path(s + ".status");
  remove_all_of({f.config, f.epoch, f.kill, f.journal_dir, f.status, f.config + ".log"});
  return f;
}

std::string engine_name(const SessionFiles& f) {
  return std::filesystem::path(f.config).stem().string();
}

// Both venues both instruments, xmm quoting instrument 0 on "quote" and hedging on instrument 1 on
// "hedge", the store on (sqlite under journal_dir), positions restored at start. `extra` is
// appended (a [risk.underlying] table, strategy parameters).
void write_xmm_config(const SessionFiles& f,
                      const ServerFixture& quote,
                      const ServerFixture& hedge,
                      const std::string& max_unhedged = "0.003",
                      const std::string& extra = {}) {
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
  text += venue_toml("quote", quote);
  text += venue_toml("hedge", hedge);
  text += instrument_toml("quote");
  text += instrument_toml("hedge");
  text += R"([strategy]
name = "xmm"

[strategy.params]
quote_instrument = 0
hedge_instrument = 1
quote_qty = 0.001
edge_bps = 2
slippage_bps = 1
hedge_tolerance_bps = 20
basis_halflife_s = 30
requote_threshold_ticks = 50
stale_ms = 5000
uncertain_hold_ms = 3000
)";
  text += "max_unhedged = " + max_unhedged + "\n";
  text += R"(
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
  text += extra;
  std::ofstream out(f.config, std::ios::trunc);
  REQUIRE(out.good());
  out << text;
}

std::string log_of(const SessionFiles& f) {
  std::error_code ec;
  if (!std::filesystem::exists(f.config + ".log", ec)) return {};
  return fastmm::test::read_file(f.config + ".log");
}

std::size_t count_of(const std::string& text, std::string_view what) {
  std::size_t n = 0;
  for (std::size_t p = text.find(what); p != std::string::npos; p = text.find(what, p + 1)) ++n;
  return n;
}

// The quote venue's position plus the hedge venue's (both in BTC).
Qty net_position(const ServerFixture& q, const ServerFixture& h) {
  return q.server.stats().position + h.server.stats().position;
}

// Orders resting on `fx` whose client order id is of session epoch `epoch`.
std::vector<sim::server::SimExchangeServer::OpenOrder> resting_of(const ServerFixture& fx,
                                                                  std::uint16_t epoch) {
  std::vector<sim::server::SimExchangeServer::OpenOrder> out;
  for (const auto& o : fx.server.open_orders()) {
    const auto cl = decode_cl_ord_id(o.client_order_id);
    if (cl && cl_ord_id_epoch(*cl) == epoch) out.push_back(o);
  }
  return out;
}

// The epoch of the orders resting on `fx` that are not of `other` (0: none resting).
std::uint16_t resting_epoch(const ServerFixture& fx, std::uint16_t other = 0) {
  for (const std::string& id : fx.server.open_client_order_ids()) {
    const auto cl = decode_cl_ord_id(id);
    if (cl && cl_ord_id_epoch(*cl) != other) return cl_ord_id_epoch(*cl);
  }
  return 0;
}

// A session is quoting both sides on the quote venue and the hedge venue's private channels are
// up; returns its epoch.
std::uint16_t wait_quoting(const ServerFixture& q,
                           const ServerFixture& h,
                           const SessionFiles& f,
                           std::uint16_t previous = 0) {
  std::uint16_t epoch = 0;
  REQUIRE_MESSAGE(wait_until(
                      [&] {
                        epoch = resting_epoch(q, previous);
                        return epoch != 0 && resting_of(q, epoch).size() == 2 &&
                               h.server.stats().user_subscriptions >= 1;
                      },
                      45000),
                  "the session never quoted both sides: " << log_of(f));
  return epoch;
}

// Fills the first resting order of `epoch` on `fx`, or the first one on `side` when given.
Qty fill_resting(ServerFixture& fx, std::uint16_t epoch, std::optional<Side> side = std::nullopt) {
  for (const auto& o : resting_of(fx, epoch)) {
    if (!side || o.side == *side) return fx.server.fill_open_order(o.client_order_id, Qty{});
  }
  return Qty{};
}

// What the engine's store says each venue's BTCUSDT position is at the end of the last session.
struct StoredPositions {
  Qty quote{};
  Qty hedge{};
};
StoredPositions stored_positions(const SessionFiles& f) {
  store::register_builtin_backends();
  auto reader = store::StoreRegistry::instance().make_reader("sqlite");
  REQUIRE(reader != nullptr);
  store::BackendOptions opts;
  opts.engine_name = engine_name(f);
  opts.default_dir = f.journal_dir;
  opts.read_only = true;
  REQUIRE(reader->open(opts).has_value());
  store::QueryFilter qf;
  qf.engine = engine_name(f);
  auto rec = reader->recovery(qf);
  REQUIRE(rec.has_value());
  REQUIRE(rec->found);
  StoredPositions s;
  for (const store::Recovery::PositionState& p : rec->position_state) {
    if (p.symbol != "BTCUSDT") continue;
    if (p.venue == "quote") s.quote = Qty::from_raw(p.qty_raw);
    if (p.venue == "hedge") s.hedge = Qty::from_raw(p.qty_raw);
  }
  return s;
}

// A fastmm-live child that a failed REQUIRE does not leave running.
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

void stop(Child& c) {
  REQUIRE(::kill(c.pid, SIGTERM) == 0);
  CHECK(reap(c.pid) == live::kExitOk);
  c.pid = -1;
}

void crash(Child& c) {
  REQUIRE(::kill(c.pid, SIGKILL) == 0);
  CHECK(reap(c.pid) == -1);  // signalled: no shutdown ran, nothing was cancelled
  c.pid = -1;
}

// A bounded look for something that must not happen (a second hedge): xmm re-evaluates the
// positions every 100 ms, so 30 of its timer periods after the restarted session quotes again.
void observe_quiet_period() {
  std::this_thread::sleep_for(std::chrono::seconds(3));
}

// After the restart: one more maker fill is hedged exactly once and the venues net to zero, which
// only happens when the engine's two positions agree with the venues'.
void one_more_round(ServerFixture& qv, ServerFixture& hv, std::uint16_t epoch) {
  const std::uint64_t before = hv.server.stats().orders_accepted;
  REQUIRE(fill_resting(qv, epoch).is_positive());
  CHECK(wait_until(
      [&] {
        return hv.server.stats().orders_accepted == before + 1 && net_position(qv, hv).is_zero();
      },
      10000));
  observe_quiet_period();
  CHECK(hv.server.stats().orders_accepted == before + 1);
  CHECK(net_position(qv, hv).is_zero());
}

}  // namespace

// 1. A maker fill arrives and the hedge IOC goes out; the hedge venue carries it out but holds its
// reply and the execution report (ack delay), so the process has not heard anything about the
// hedge when it is killed. The restarted process restores the quote position from the store,
// replays the hedge venue's executions, finds the hedge done and sends no second one.
TEST_CASE("xmm restart: SIGKILL between the hedge going out and its ack; no second hedge") {
  ServerFixture qv(quiet(7));
  ServerFixture hv(quiet(11));
  const SessionFiles f = session_files("xmm-crash-hedge");
  write_xmm_config(f, qv, hv);

  Child first(spawn_live(f, 300));
  const std::uint16_t e1 = wait_quoting(qv, hv, f);
  hv.server.set_ack_delay_ms(600'000);  // the hedge executes; the process hears nothing
  const Qty fill1 = fill_resting(qv, e1);
  REQUIRE(fill1.is_positive());
  REQUIRE_MESSAGE(wait_until([&] { return hv.server.stats().orders_accepted == 1; }, 10000),
                  "no hedge: " << log_of(f));
  crash(first);
  hv.server.set_ack_delay_ms(0);
  // The hedge happened at the venue, and nothing else: the dead process's other quote still rests.
  CHECK(hv.server.stats().orders_accepted == 1);
  CHECK(net_position(qv, hv).is_zero());

  Child second(spawn_live(f, 300));
  const std::uint16_t e2 = wait_quoting(qv, hv, f, e1);
  CHECK(e2 != e1);
  observe_quiet_period();
  INFO("second session: " << log_of(f));
  CHECK(hv.server.stats().orders_accepted == 1);  // no second hedge for the fill
  CHECK(net_position(qv, hv).is_zero());
  CHECK(resting_of(qv, e1).empty());  // the dead session's quote was cancelled

  one_more_round(qv, hv, e2);
  stop(second);
  const StoredPositions s = stored_positions(f);
  CHECK(s.quote == qv.server.stats().position);
  CHECK(s.hedge == hv.server.stats().position);
  CHECK(booked_twice(f, engine_name(f)).empty());
  CHECK(qv.server.stats().duplicate_client_order_ids == 0);
  CHECK(hv.server.stats().duplicate_client_order_ids == 0);
}

// 2. The maker fill happens while the process is dead: a quote left resting at the kill is filled
// by the simulator during the downtime. The restarted process's execution replay books it and xmm
// hedges it exactly once. `trade_first` gives the first session a hedged round before the crash,
// so its store has fills to resume from; without it the store has none.
void fill_while_down(bool trade_first, const std::string& stem) {
  ServerFixture qv(quiet(7));
  ServerFixture hv(quiet(11));
  const SessionFiles f = session_files(stem);
  write_xmm_config(f, qv, hv);

  Child first(spawn_live(f, 300));
  std::uint16_t e1 = wait_quoting(qv, hv, f);
  if (trade_first) {
    REQUIRE(fill_resting(qv, e1).is_positive());
    REQUIRE(wait_until(
        [&] {
          return hv.server.stats().orders_accepted == 1 && net_position(qv, hv).is_zero() &&
                 resting_of(qv, e1).size() == 2;
        },
        10000));
  }
  const std::uint64_t hedges_before = hv.server.stats().orders_accepted;
  crash(first);
  const Qty filled = fill_resting(qv, e1);  // nobody running
  REQUIRE(filled.is_positive());
  REQUIRE_FALSE(net_position(qv, hv).is_zero());

  Child second(spawn_live(f, 300));
  REQUIRE_MESSAGE(wait_until(
                      [&] {
                        return hv.server.stats().orders_accepted == hedges_before + 1 &&
                               net_position(qv, hv).is_zero();
                      },
                      30000),
                  "the fill made while down was not hedged: " << log_of(f));
  const std::uint16_t e2 = wait_quoting(qv, hv, f, e1);
  observe_quiet_period();
  INFO("second session: " << log_of(f));
  CHECK(hv.server.stats().orders_accepted == hedges_before + 1);
  CHECK(net_position(qv, hv).is_zero());

  one_more_round(qv, hv, e2);
  stop(second);
  const StoredPositions s = stored_positions(f);
  CHECK(s.quote == qv.server.stats().position);
  CHECK(s.hedge == hv.server.stats().position);
  CHECK(booked_twice(f, engine_name(f)).empty());
  CHECK(qv.server.stats().duplicate_client_order_ids == 0);
  CHECK(hv.server.stats().duplicate_client_order_ids == 0);
}

TEST_CASE("xmm restart: a quote filled while the process is down is hedged once") {
  fill_while_down(true, "xmm-down-fill");
}

TEST_CASE("xmm restart: a fill while down is hedged once when the dead session never traded") {
  fill_while_down(false, "xmm-down-first");
}

// 4. `[risk.underlying.BTC] max_net` across a crash. The quote left resting is filled while the
// process is down, and every hedge the restarted process sends is refused by the hedge venue, so
// the fill stays unhedged. Only the restarted process's knowledge of that fill keeps it from
// quoting the side that adds to it: the test fills every order it rests at once, and the venues'
// net never goes past max_net. (xmm's own max_unhedged is set wide so the engine's limit is the one
// that holds.)
TEST_CASE("xmm restart: the restarted process keeps [risk.underlying] max_net while re-hedging") {
  ServerFixture qv(quiet(7));
  ServerFixture hv(quiet(11));
  const SessionFiles f = session_files("xmm-crash-risk");
  write_xmm_config(f, qv, hv, "0.01", "\n[risk.underlying.BTC]\nmax_net = 0.0015\n");

  Child first(spawn_live(f, 300));
  const std::uint16_t e1 = wait_quoting(qv, hv, f);
  crash(first);
  const Qty filled = fill_resting(qv, e1, Side::Buy);
  REQUIRE(filled.is_positive());
  hv.server.reject_next_orders(1000);

  Child second(spawn_live(f, 300));
  // Whatever the restarted session rests on the quote venue is filled at once, for a while.
  Qty worst = net_position(qv, hv).abs();
  std::uint64_t taken = 0;
  std::uint16_t e2 = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
  while (std::chrono::steady_clock::now() < deadline) {
    if (e2 == 0) e2 = resting_epoch(qv, e1);
    if (e2 != 0) {
      for (const auto& o : resting_of(qv, e2)) {
        if (qv.server.fill_open_order(o.client_order_id, Qty{}).is_positive()) ++taken;
        worst = std::max(worst, net_position(qv, hv).abs());
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  INFO("second session: " << log_of(f));
  const sim::server::SimServerStats hs = hv.server.stats();
  CHECK(hs.orders_rejected >= 1);  // it tried to hedge
  CHECK(hs.orders_accepted == 0);
  CHECK(taken >= 1);  // it quoted the side that reduces the position, and that traded
  CHECK(worst <= Qty::from_double(0.0015));
  stop(second);
  const StoredPositions s = stored_positions(f);
  CHECK(s.quote == qv.server.stats().position);
  CHECK(s.hedge == hv.server.stats().position);
}

#ifdef FASTMM_GATEWAY_EXE

// 3. The same crash behind fastmm-gateway: the strategy process is killed while its hedge is out
// and unanswered; the gateway (and its venue connections) live on. The detach cancels the dead
// strategy's quotes; the held reply and execution report reach the gateway only after the
// restarted strategy has attached, and name an order of an epoch nobody holds. The restarted
// strategy books the hedge once (its attach replays the hedge venue's executions) and hedges
// nothing more.
TEST_CASE("xmm restart: behind fastmm-gateway, SIGKILL of the strategy with its hedge unanswered") {
  ServerFixture qv(quiet(7));
  ServerFixture hv(quiet(11));
  const SessionFiles fg = session_files("xmm-gw-gateway");
  const SessionFiles f = session_files("xmm-gw-strategy");
  write_xmm_config(fg, qv, hv);
  write_xmm_config(f, qv, hv);
  // A socket path fits in 107 bytes wherever the build tree is.
  GatewayProcess g;
  g.socket = "/tmp/fastmm-xmm-gw-" + std::to_string(::getpid()) + ".sock";
  g.log = fg.config + ".gw.log";
  remove_all_of({g.socket, g.log});
  g.pid = spawn_process(FASTMM_GATEWAY_EXE,
                        {"--config", fg.config, "--socket", g.socket, "--log", g.log});
  Child gw(g.pid);
  REQUIRE_MESSAGE(wait_until(
                      [&] {
                        const auto q = qv.server.stats();
                        const auto h = hv.server.stats();
                        return std::filesystem::exists(g.socket) && q.user_subscriptions >= 1 &&
                               h.user_subscriptions >= 1 && q.md_sessions >= 1 &&
                               h.md_sessions >= 1;
                      },
                      30000),
                  "the gateway did not come up: " << fastmm::test::read_file(g.log));

  Child first(spawn_strategy(f, g, 300));
  const std::uint16_t e1 = wait_quoting(qv, hv, f);
  constexpr int kHeldMs = 8000;
  hv.server.set_ack_delay_ms(kHeldMs);
  const auto held_at = std::chrono::steady_clock::now();
  REQUIRE(fill_resting(qv, e1).is_positive());
  REQUIRE_MESSAGE(wait_until([&] { return hv.server.stats().orders_accepted == 1; }, 10000),
                  "no hedge: " << log_of(f));
  crash(first);
  hv.server.set_ack_delay_ms(0);  // for what comes next; the hedge's reply stays held
  // The detach cancelled the dead strategy's other quote.
  CHECK(wait_until([&] { return resting_of(qv, e1).empty(); }, 10000));

  Child second(spawn_strategy(f, g, 300));
  const std::uint16_t e2 = wait_quoting(qv, hv, f, e1);
  CHECK(e2 != e1);
  CHECK(std::chrono::steady_clock::now() - held_at < std::chrono::milliseconds(kHeldMs));
  // Past the held reply and execution report, then a quiet period.
  std::this_thread::sleep_until(held_at + std::chrono::milliseconds(kHeldMs));
  observe_quiet_period();
  INFO("strategy: " << log_of(f) << "\ngateway: " << fastmm::test::read_file(g.log));
  CHECK(hv.server.stats().orders_accepted == 1);
  CHECK(net_position(qv, hv).is_zero());

  one_more_round(qv, hv, e2);
  stop(second);
  const StoredPositions s = stored_positions(f);
  CHECK(s.quote == qv.server.stats().position);
  CHECK(s.hedge == hv.server.stats().position);
  CHECK(booked_twice(f, engine_name(f)).empty());
  stop(gw);  // its log is complete once it has exited
  // The gateway's account (what [gateway] limits are checked against) holds the venues' positions
  // too: each detach logs them, quote venue first; the second is the restarted strategy's.
  {
    const std::string gl = fastmm::test::read_file(g.log);
    const std::string key = "the account keeps the positions of ";
    CHECK(count_of(gl, key) == 2);
    const std::size_t at = gl.rfind(key);
    REQUIRE(at != std::string::npos);
    const std::size_t q = gl.find("BTCUSDT=", at);
    REQUIRE(q != std::string::npos);
    const std::size_t h = gl.find("BTCUSDT=", q + 8);
    REQUIRE(h != std::string::npos);
    CHECK(std::strtod(gl.c_str() + q + 8, nullptr) ==
          doctest::Approx(qv.server.stats().position.to_double()));
    CHECK(std::strtod(gl.c_str() + h + 8, nullptr) ==
          doctest::Approx(hv.server.stats().position.to_double()));
  }
  CHECK(qv.server.stats().duplicate_client_order_ids == 0);
  CHECK(hv.server.stats().duplicate_client_order_ids == 0);
}

#endif  // FASTMM_GATEWAY_EXE

#endif  // FASTMM_LIVE_EXE
