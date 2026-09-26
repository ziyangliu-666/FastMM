// xmm in a live session (fastmm::live::run_live, split threading) against two in-process
// Binance-compatible simulators: quotes on one, hedges on the other. Venue-side facts decide: the
// hedge venue's accepted orders count the hedges, and the two venues' positions must add up to
// zero. A maker fill leads to exactly one hedge; a hedge whose reply and events are lost while the
// hedge venue's connections drop is settled by the reconnect (execution replay, reconciliation)
// without a second hedge, and the next maker fill is hedged once again.
#include "integration_util.hpp"

#include "fastmm/live/session.hpp"
#include "fastmm/strategies/builtin.hpp"
#include "fastmm/strategies/registry.hpp"

#include <signal.h>
#include <unistd.h>

#include <filesystem>
#include <sstream>
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

Config session_config(const ServerFixture& quote, const ServerFixture& hedge) {
  std::string text = R"([engine]
name = "xmm-live"
cpu = -1
net_cpus = []
spin_mode = "adaptive"
journal = false
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
  text += "epoch_file = \"" + fresh("xmm-live.epoch") + "\"\n";
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
quote_fee_bps = 1
hedge_fee_bps = 4
slippage_bps = 1
hedge_tolerance_bps = 20
basis_halflife_s = 30
max_unhedged = 0.003
requote_threshold_ticks = 50
stale_ms = 5000
uncertain_hold_ms = 3000

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

# No store: a previous run's executions (the simulators restart their trade ids) would be taken
# for ones already booked and the replay after the drop would skip the hedge's fill.
[storage]
backend = "none"

[logging]
level = "info"
file = ""
mirror_level = "warn"
)";
  Config::LoadOptions opts;
  opts.substitute_env = false;
  return Config::parse(text, opts, "xmm-live.toml");
}

// Our open orders on the quote venue.
std::size_t quotes(const ServerFixture& fx) {
  return fx.server.open_client_order_ids().size();
}

// The quote venue's position plus the hedge venue's (both in BTC).
Qty net_position(const ServerFixture& q, const ServerFixture& h) {
  return q.server.stats().position + h.server.stats().position;
}

// Fills one of our quotes on the quote venue in full; its quantity.
Qty fill_a_quote(ServerFixture& q) {
  const std::vector<std::string> ids = q.server.open_client_order_ids();
  if (ids.empty()) return Qty{};
  return q.server.fill_open_order(ids.front(), Qty{});
}

struct Outcome {
  bool quoting = false;
  // 1: a maker fill and its hedge
  Qty fill1{};
  bool hedged1 = false;
  std::uint64_t hedges1 = 0;
  Qty net1{};
  // 2: the hedge in the dark, the hedge venue drops
  Qty fill2{};
  bool executed2 = false;
  std::uint64_t swallowed2 = 0;
  bool requoted2 = false;
  std::uint64_t hedges2 = 0;
  Qty net2{};
  // 3: the next fill is hedged once
  Qty fill3{};
  bool hedged3 = false;
  std::uint64_t hedges3 = 0;
  Qty net3{};
  std::uint64_t duplicate_ids = 0;
};

// Gives the session time to do something it should not.
void settle(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace

TEST_CASE("xmm: quote on one venue, hedge on the other, through a hedge venue drop") {
  static const bool registered = [] {
    register_builtin_strategies(StrategyRegistry::instance());
    return true;
  }();
  static_cast<void>(registered);
  ServerFixture qv(quiet(7));
  ServerFixture hv(quiet(11));
  const Config cfg = session_config(qv, hv);
  live::LiveOptions o;
  o.duration_ns = seconds(90).ns;  // a bound, not the expected end
  o.no_journal = true;
  o.no_status = true;
  o.no_control = true;
  o.program = "xmm-live-test";

  Outcome r;
  std::thread driver([&] {
    auto accepted = [&] { return hv.server.stats().orders_accepted; };
    r.quoting = wait_until([&] { return quotes(qv) == 2; }, 30000);
    if (r.quoting) {
      // 1. A maker fill: exactly one hedge, and the venues net to zero.
      r.fill1 = fill_a_quote(qv);
      r.hedged1 =
          wait_until([&] { return accepted() >= 1 && net_position(qv, hv).is_zero(); }, 10000);
      settle(1500);
      r.hedges1 = accepted();
      r.net1 = net_position(qv, hv);

      // 2. The hedge goes out and executes, but its reply is swallowed and the private stream is
      // muted: the engine does not know. Then the hedge venue's order and user connections drop.
      static_cast<void>(wait_until([&] { return quotes(qv) == 2; }, 10000));
      hv.server.set_user_stream_muted(true);
      hv.server.swallow_next_ws_api_responses(1);
      r.fill2 = fill_a_quote(qv);
      r.executed2 = wait_until([&] { return accepted() >= r.hedges1 + 1; }, 10000);
      settle(300);
      r.swallowed2 = hv.server.stats().responses_swallowed;
      hv.server.set_user_stream_muted(false);
      hv.server.drop_ws_api_connections(true);
      // Reconnect, replay, reconcile; the quotes come back once the hedge venue is Live again.
      settle(1000);
      r.requoted2 = wait_until([&] { return quotes(qv) == 2; }, 20000);
      settle(4000);  // past uncertain_hold_ms: a second hedge would have gone by now
      r.hedges2 = accepted();
      r.net2 = net_position(qv, hv);

      // 3. The next maker fill is hedged exactly once: the engine's positions agree with the
      // venues'.
      r.fill3 = fill_a_quote(qv);
      r.hedged3 = wait_until(
          [&] { return accepted() >= r.hedges2 + 1 && net_position(qv, hv).is_zero(); }, 10000);
      settle(1500);
      r.hedges3 = accepted();
      r.net3 = net_position(qv, hv);
    }
    r.duplicate_ids = hv.server.stats().duplicate_client_order_ids;
    ::kill(::getpid(), SIGTERM);  // run_live's handler: the normal shutdown
  });
  const int rc = live::run_live(cfg, o);
  driver.join();

  const auto qs = qv.server.stats();
  const auto hs = hv.server.stats();
  INFO("quote venue: accepted=" << qs.orders_accepted << " fills=" << qs.fills
                                << " position=" << qs.position);
  INFO("hedge venue: accepted=" << hs.orders_accepted << " rejected=" << hs.orders_rejected
                                << " fills=" << hs.fills << " position=" << hs.position);
  CHECK(rc == live::kExitOk);
  REQUIRE(r.quoting);
  CHECK(r.fill1.is_positive());
  CHECK(r.hedged1);
  CHECK(r.hedges1 == 1);
  CHECK(r.net1.is_zero());

  CHECK(r.fill2.is_positive());
  CHECK(r.executed2);
  CHECK(r.swallowed2 == 1);
  CHECK(r.requoted2);
  CHECK(r.hedges2 == 2);  // no second hedge for the fill whose hedge went unheard
  CHECK(r.net2.is_zero());

  CHECK(r.fill3.is_positive());
  CHECK(r.hedged3);
  CHECK(r.hedges3 == 3);
  CHECK(r.net3.is_zero());
  CHECK(r.duplicate_ids == 0);
  CHECK(wait_until([&] { return qv.server.stats().open_orders == 0; }, 5000));
}
