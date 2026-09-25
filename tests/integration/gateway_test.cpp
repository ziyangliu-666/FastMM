// fastmm-gateway holds the venue connections; strategy processes (fastmm-live --gateway) attach to
// it, trade through shared-memory rings and go. The simulator runs in this process, the gateway and
// the strategies are real children: a strategy's death has to be a real one, kill -9 included.
#include "fastmm/live/gateway.hpp"

#include "gateway_util.hpp"

#include "fastmm/core/session_state.hpp"
#include "fastmm/live/session.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

#if defined(FASTMM_LIVE_EXE) && defined(FASTMM_GATEWAY_EXE)

TEST_CASE("gateway: a strategy attached through the gateway trades and its position agrees") {
  ServerFixture fx;
  const SessionFiles f = write_config(fx, "gateway-trade", "exit", "1000");
  remove_all_of({f.epoch, f.kill, f.journal_dir, f.config + ".log"});
  const GatewayProcess g = spawn_gateway(f);
  wait_gateway_up(fx, g);
  const Sessions opened = sessions_opened(fx);

  const pid_t strategy = spawn_strategy(f, g);
  REQUIRE_MESSAGE(wait_until([&] { return fx.server.stats().fills >= 3; }, 60000),
                  "the strategy never traded: " << fastmm::test::read_file(f.config + ".log"));
  REQUIRE(::kill(strategy, SIGTERM) == 0);
  CHECK(reap(strategy) == live::kExitOk);
  // The strategy cancelled through the gateway on its way out, and the gateway cancels all on the
  // detach.
  CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));

  const std::string log = fastmm::test::read_file(f.config + ".log");
  CHECK(log.find("gateway: attached to") != std::string::npos);
  CHECK(log.find("no venue cancel_all here") != std::string::npos);
  const sim::server::SimServerStats ss = fx.server.stats();
  INFO("venue position " << ss.position.raw << " fills " << ss.fills);
  CHECK(store_position(f, "gateway-trade") == ss.position);
  CHECK(ss.duplicate_client_order_ids == 0);
  // The engine traded with the gateway's reference data and never opened a venue session itself.
  const Sessions after = sessions_opened(fx);
  CHECK(after.md == opened.md);
  CHECK(after.api == opened.api);
  stop_gateway(g);
}

TEST_CASE(
    "gateway: kill -9 of a strategy cancels its orders at once and the next one reconciles and "
    "trades") {
  ServerFixture fx;
  const SessionFiles f = write_config(fx, "gateway-crash", "exit", "1000");
  remove_all_of({f.epoch, f.kill, f.journal_dir, f.config + ".log"});
  const GatewayProcess g = spawn_gateway(f);
  wait_gateway_up(fx, g);
  const Sessions opened = sessions_opened(fx);

  // First strategy: trade, and have quotes resting at the venue when it dies.
  const pid_t first = spawn_strategy(f, g);
  REQUIRE_MESSAGE(
      wait_until(
          [&] {
            if (fx.server.stats().fills < 1) return false;
            auto st = KillStateStore::load(f.kill);
            return st && st->fees.is_positive();
          },
          60000),
      "the first strategy never traded: " << fastmm::test::read_file(f.config + ".log"));
  // An instrument has one strategy: a second attach claiming it is refused while the first lives.
  {
    std::string err;
    live::GatewayAttachRequest req;
    req.engine = "intruder";
    req.instruments = {{"sim", "BTCUSDT"}};
    auto second = live::GatewayClient::attach(g.socket, req, &err);
    CHECK(second == nullptr);
    CHECK(err.find("BTCUSDT on venue 'sim' is traded by gateway-crash") != std::string::npos);
  }
  REQUIRE(wait_until([&] { return fx.server.stats().open_orders > 0; }, 20000));

  REQUIRE(::kill(first, SIGKILL) == 0);
  const auto killed_at = std::chrono::steady_clock::now();
  const bool cleared = wait_until([&] { return fx.server.stats().open_orders == 0; }, 2000);
  const auto cleared_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - killed_at)
                              .count();
  CHECK(reap(first) == -1);  // signalled: no shutdown of its own ran
  CHECK_MESSAGE(cleared, "orders still resting 2 s after the strategy died");
  MESSAGE("kill -9 -> no open orders at the venue in " << cleared_ms << " ms");
  // The venue sessions stayed up: nothing reconnected.
  Sessions now = sessions_opened(fx);
  CHECK(now.md == opened.md);
  CHECK(now.api == opened.api);

  // Second strategy on the same state files: it restores the position from its store, the gateway
  // replays the venue's executions since then into it, and it trades.
  const sim::server::SimServerStats before = fx.server.stats();
  const pid_t second = spawn_strategy(f, g);
  REQUIRE_MESSAGE(
      wait_until(
          [&] {
            const sim::server::SimServerStats s = fx.server.stats();
            return s.orders_accepted > before.orders_accepted && s.fills > before.fills;
          },
          60000),
      "the second strategy never traded: " << fastmm::test::read_file(f.config + ".log"));
  REQUIRE(::kill(second, SIGTERM) == 0);
  CHECK(reap(second) == live::kExitOk);
  CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));

  const sim::server::SimServerStats ss = fx.server.stats();
  INFO("venue position " << ss.position.raw << " before the second strategy "
                         << before.position.raw);
  CHECK(store_position(f, "gateway-crash") == ss.position);
  CHECK(ss.duplicate_client_order_ids == 0);
  now = sessions_opened(fx);
  CHECK(now.md == opened.md);
  CHECK(now.api == opened.api);
  stop_gateway(g);
  // Both detaches cancelled (the log file is complete once the gateway has exited).
  const std::string gw_log = fastmm::test::read_file(g.log);
  CHECK(gw_log.find("attachment 1, epoch 1) detached after") != std::string::npos);
  CHECK(gw_log.find("attachment 2, epoch 2) detached after") != std::string::npos);
  CHECK(gw_log.find("cancel_all FAILED") == std::string::npos);
}

#endif  // FASTMM_LIVE_EXE && FASTMM_GATEWAY_EXE
