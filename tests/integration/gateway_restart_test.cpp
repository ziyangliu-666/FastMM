// A gateway restart while a strategy's order rests: kill -9 of the gateway (its strategies exit on
// the loss), the order fills at the venue while nothing runs, a new gateway comes up and does its
// start-up sweep, then the strategies attach again. The fill is its strategy's: its store holds it
// once, naming the order, and the gateway's account books it once, towards that strategy.
//
// The simulator's trade history, like Binance's, gives the venue's order id and not the client
// order id; the new gateway never saw the order, so the strategy's store says whose it is, or the
// venue (GET /api/v3/order) for an order it never acknowledged. The last case: a fill streamed to
// the new gateway before its start-up sweep, of a session no strategy has claimed yet.
#include "gateway_util.hpp"

#include "fastmm/core/status_segment.hpp"

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

#if defined(FASTMM_LIVE_EXE) && defined(FASTMM_GATEWAY_EXE)

namespace {

struct Plan {
  bool shared = false;  // a and b both trade BTCUSDT ([gateway.shared]); else b trades BTCUSDC
  bool a_first = true;  // a attaches to the new gateway before b; else b's replay runs first
  std::int64_t venue_ahead_ms = 0;  // the venue's clock ahead of the host's
  // The order that fills is one a sent last, which the venue placed and never answered (no reply,
  // no execution report): a's store has no venue order id for it. a is killed -9 before the
  // gateway.
  bool unacked = false;
  // b is the shared instrument's primary, and the new gateway's first order lookup fails: the fill
  // goes out naming no order before a later replay names it.
  bool primary_b = false;
};

// The simulator of a restart. Every fill is one the test makes. The new gateway's start-up sweep
// waits for its execution replay, a bulk query, and the first gateway and its strategies have spent
// some 430 to 575 of the IP's weight by then: past the bulk share of a 6000 limit at the start of
// a minute (RateLimiter::kBulkPaceFloor), so a restart in a minute's first seconds (of the venue's
// clock) swept some 13 s later. Rate limits are not what these test.
sim::server::SimServerConfig restart_server() {
  sim::server::SimServerConfig sc = two_markets();
  sc.generator.market_rate_per_s = 0.0;
  sc.weight_limit_per_minute = 60000;
  return sc;
}

void restart_with_fill(const std::string& stem, const Plan& p) {
  sim::server::SimServerConfig sc = restart_server();
  sc.clock_offset_ms = p.venue_ahead_ms;
  ServerFixture fx(sc);
  Configs c = write_configs(fx, stem);
  if (p.shared) {
    // Both trade BTCUSDT, which the gateway lists under [gateway.shared].
    c.b = write_config(fx, c.b_name, "exit", "1000");
    rewrite(c.gw.config, [&](std::string& t) {
      t += "\n[gateway.shared.\"sim:BTCUSDT\"]\n";
      if (p.primary_b) t += "primary = \"" + c.b_name + "\"\n";
    });
    remove_all_of({c.b.epoch, c.b.kill, c.b.journal_dir, c.b.config + ".log", c.b.status});
  }
  const std::string gw_name = stem + "-gw";
  remove_all_of({default_gateway_status_path(gw_name)});
  const std::size_t b_symbol = p.shared ? 0 : 1;

  GatewayProcess g = spawn_gateway(c.gw);
  wait_gateway_up(fx, g);
  const pid_t a = spawn_strategy(c.a, g);
  const std::uint16_t ea = wait_resting(fx, c.a, {});
  const pid_t b = spawn_strategy(c.b, g);
  const std::uint16_t eb = wait_resting(fx, c.b, {ea});
  // Each has a fill in its store: its next attach resumes after it.
  REQUIRE_MESSAGE(fill_one_of(fx, ea).is_positive(), fastmm::test::read_file(c.a.config + ".log"));
  REQUIRE_MESSAGE(fill_one_of(fx, eb).is_positive(), fastmm::test::read_file(c.b.config + ".log"));
  REQUIRE(wait_until(
      [&] {
        const auto sa = try_stored(c.a, c.a_name);
        const auto sb = try_stored(c.b, c.b_name);
        return sa && !sa->exec_ids.empty() && sb && !sb->exec_ids.empty() && open_of(fx, ea) > 0 &&
               open_of(fx, eb) > 0;
      },
      20000));

  std::string dead_order;
  if (p.unacked) {
    // Nothing comes back from the venue any more: a's next order (a requote) is placed there and
    // nobody hears of it. Then a dies.
    const std::vector<std::string> answered = fx.server.open_client_order_ids();
    fx.server.set_user_stream_muted(true);
    fx.server.swallow_next_ws_api_responses(1'000'000);
    REQUIRE_MESSAGE(wait_until(
                        [&] {
                          for (const std::string& id : fx.server.open_client_order_ids()) {
                            const auto cl = decode_cl_ord_id(id);
                            if (cl && cl_ord_id_epoch(*cl) == ea && !holds(answered, id)) {
                              dead_order = id;
                              return true;
                            }
                          }
                          return false;
                        },
                        20000),
                    "a placed nothing more: " << fastmm::test::read_file(c.a.config + ".log"));
  }
  // The gateway dies; the strategies exit on its loss (a is killed -9 as well when its order went
  // unanswered), and their orders stay at the venue. The gateway goes first: a's detach would have
  // it cancel a's orders.
  REQUIRE(::kill(g.pid, SIGKILL) == 0);
  if (p.unacked) REQUIRE(::kill(a, SIGKILL) == 0);
  CHECK(reap(g.pid) == -1);
  CHECK(reap(a) != live::kExitOk);
  CHECK(reap(b) != live::kExitOk);
  fx.server.set_user_stream_muted(false);
  fx.server.swallow_next_ws_api_responses(0);
  REQUIRE(wait_until(
      [&] {
        const sim::server::SimServerStats s = fx.server.stats();
        return s.md_sessions == 0 && s.api_sessions == 0 && s.user_subscriptions == 0 &&
               (p.unacked ? holds(fx.server.open_client_order_ids(), dead_order)
                          : open_of(fx, ea) > 0);
      },
      10000));
  const Qty dead_fill =
      p.unacked ? fx.server.fill_open_order(dead_order) : fill_one_of(fx, ea, &dead_order);
  REQUIRE(dead_fill.is_positive());

  // The new gateway: up, and its start-up sweep has cancelled the orders nobody holds.
  if (p.primary_b) fx.server.fail_next_order_queries(1);
  g = spawn_gateway(c.gw);
  wait_gateway_up(fx, g);
  REQUIRE_MESSAGE(wait_until([&] { return fx.server.stats().open_orders == 0; }, 10000),
                  fastmm::test::read_file(g.log));
  // Then the strategies, one after the other: each rests orders only once its attach's replay and
  // reconciliation are done.
  pid_t a2 = -1;
  pid_t b2 = -1;
  std::uint16_t ea2 = 0;
  std::uint16_t eb2 = 0;
  if (p.a_first) {
    a2 = spawn_strategy(c.a, g);
    ea2 = wait_resting(fx, c.a, {ea, eb});
    b2 = spawn_strategy(c.b, g);
    eb2 = wait_resting(fx, c.b, {ea, eb, ea2});
  } else {
    b2 = spawn_strategy(c.b, g);
    eb2 = wait_resting(fx, c.b, {ea, eb});
    a2 = spawn_strategy(c.a, g);
    ea2 = wait_resting(fx, c.a, {ea, eb, eb2});
  }
  CHECK_MESSAGE(wait_until([&] { return stored_holds(c.a, c.a_name, dead_order); }, 10000),
                "a never booked the fill of " << dead_order << " made while the gateway was down: "
                                              << fastmm::test::read_file(c.a.config + ".log")
                                              << fastmm::test::read_file(g.log));
  // The gateway's account: the venue's position, nothing unattributed or unexplained.
  std::optional<StatusPosition> btc;
  Qty venue_btc{};
  const bool agreed = wait_until(
      [&] {
        btc = gateway_position(gw_name);
        venue_btc = position(fx.server.stats(), 0);
        return btc && btc->qty_raw == venue_btc.raw && btc->unattributed_raw == 0 &&
               btc->unexplained_raw == 0;
      },
      5000);
  REQUIRE(btc.has_value());
  INFO("gateway BTCUSDT " << btc->qty_raw << " unattributed " << btc->unattributed_raw
                          << " unexplained " << btc->unexplained_raw << "; venue "
                          << venue_btc.raw);
  CHECK(agreed);

  stop_strategy(a2);
  stop_strategy(b2);
  CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
  stop_gateway(g);

  // a's store holds the fill once; the stores together are the venue.
  const StoredFills fa = stored(c.a, c.a_name);
  const StoredFills fb = stored(c.b, c.b_name);
  CHECK(std::count(fa.cl_ord_ids.begin(), fa.cl_ord_ids.end(), dead_order) == 1);
  CHECK(!holds(fb.cl_ord_ids, dead_order));
  // No execution in both stores, under any order's name or none (the primary receives what
  // names none).
  for (const std::string& exec : fa.exec_ids) {
    INFO("trade id " << exec);
    CHECK(!holds(fb.exec_ids, exec));
  }
  if (p.primary_b) {
    // The lookup did fail first.
    CHECK(fastmm::test::read_file(g.log).find("its fill names no order yet") != std::string::npos);
  }
  CHECK(booked_twice(c.a, c.a_name).empty());
  CHECK(booked_twice(c.b, c.b_name).empty());
  const Qty pa = store_position(c.a, c.a_name);
  const Qty pb = store_position(c.b, c.b_name, p.shared ? "BTCUSDT" : "BTCUSDC");
  const sim::server::SimServerStats end = fx.server.stats();
  INFO("a " << pa.raw << " (fills " << fa.sum.raw << ") b " << pb.raw << " (fills " << fb.sum.raw
            << ") venue " << position(end, 0).raw << " / " << position(end, b_symbol).raw);
  CHECK(pa == fa.sum);
  CHECK(pb == fb.sum);
  if (p.shared) {
    CHECK(pa + pb == position(end, 0));
    CHECK(logged_share(g, c.a_name) == pa);
    CHECK(logged_share(g, c.b_name) == pb);
    CHECK(logged_share(g, "unattributed") == Qty{});
  } else {
    CHECK(pa == position(end, 0));
    CHECK(pb == position(end, 1));
  }
  // Neither saw the other's fills.
  for (const JournalEpochs& j : read_journals(c.b)) {
    INFO(j.path);
    CHECK(!j.fill_epochs.contains(ea));
    CHECK(!j.fill_epochs.contains(ea2));
  }
  for (const JournalEpochs& j : read_journals(c.a)) {
    INFO(j.path);
    CHECK(!j.fill_epochs.contains(eb));
    CHECK(!j.fill_epochs.contains(eb2));
  }
}

}  // namespace

TEST_CASE(
    "gateway restart: a shared instrument's fill made while the gateway was down reaches its "
    "strategy, attached first") {
  restart_with_fill("gw-rs-sh-a", Plan{true, true, 0});
}

TEST_CASE(
    "gateway restart: a shared instrument's fill another strategy's replay met first, the venue "
    "clock ahead, reaches its strategy and the account once") {
  // As in the Binance Demo run: the venue's clock ahead of the host's, so that the fill's venue
  // time was later than the new gateway's start in the host's clock, and b's replay (booking what
  // names no order and no primary for the account) met it before a had attached.
  restart_with_fill("gw-rs-sh-b", Plan{true, false, 3000});
}

TEST_CASE(
    "gateway restart: an owned instrument's fill made while the gateway was down reaches its "
    "strategy, attached first") {
  restart_with_fill("gw-rs-own-a", Plan{false, true, 0});
}

TEST_CASE(
    "gateway restart: an owned instrument's fill another strategy's replay met first reaches its "
    "strategy and the account once") {
  // b's replay meets a's fill before a's store has seeded the account's position of BTCUSDT.
  restart_with_fill("gw-rs-own-b", Plan{false, false, 3000});
}

TEST_CASE(
    "gateway restart: a shared instrument's fill of an order the venue never acknowledged reaches "
    "its strategy naming the order") {
  // The trade history names the order by the venue's id only, and neither the new gateway nor a's
  // store has that id: the connector asks the venue for the order (GET /api/v3/order).
  restart_with_fill("gw-rs-sh-u", Plan{true, true, 0, true});
}

TEST_CASE(
    "gateway restart: an owned instrument's fill of an order the venue never acknowledged reaches "
    "its strategy naming the order, another strategy's replay first") {
  restart_with_fill("gw-rs-own-u", Plan{false, false, 0, true});
}

TEST_CASE(
    "gateway restart: a fill whose order lookup failed first reaches its strategy only, not the "
    "primary") {
  // b, the primary, attaches first; its replay meets a's fills, the one made while nothing ran
  // among them, and the first order lookup fails: that fill goes out naming no order, marked to be
  // sent again (kUnresolved). The gateway holds it back from b; a later replay names the order and
  // a receives it (or its store has it). The account books it once.
  restart_with_fill("gw-rs-sh-p", Plan{true, false, 0, true, true});
}

TEST_CASE(
    "gateway restart: a streamed fill of a session nobody has claimed yet is the account's, then "
    "its strategy's when it attaches") {
  // A new gateway is connected and its start-up sweep has not answered yet (the venue holds the
  // open-order snapshot) when a's order from before the restart fills: the execution report names
  // an epoch no attached strategy holds, so the account books it for nobody (unattributed). When a
  // attaches and claims its earlier epochs, the parked fill reaches it and the account moves it to
  // a's share (AccountBook::retag).
  ServerFixture fx(restart_server());
  Configs c = write_configs(fx, "gw-rs-retag");
  rewrite(c.gw.config, [](std::string& t) { t += "\n[gateway.shared.\"sim:BTCUSDT\"]\n"; });
  const std::string gw_name = "gw-rs-retag-gw";
  remove_all_of({default_gateway_status_path(gw_name)});

  GatewayProcess g = spawn_gateway(c.gw);
  wait_gateway_up(fx, g);
  const pid_t a = spawn_strategy(c.a, g);
  const std::uint16_t ea = wait_resting(fx, c.a, {});
  REQUIRE(::kill(g.pid, SIGKILL) == 0);
  CHECK(reap(g.pid) == -1);
  CHECK(reap(a) != live::kExitOk);
  REQUIRE(wait_until(
      [&] {
        const sim::server::SimServerStats s = fx.server.stats();
        return s.md_sessions == 0 && s.api_sessions == 0 && s.user_subscriptions == 0 &&
               open_of(fx, ea) > 0;
      },
      10000));

  // The new gateway's snapshot is answered 8 s after it asks: its sweep cannot cancel a's orders
  // before then.
  fx.server.set_open_orders_delay_ms(8000);
  g = spawn_gateway(c.gw);
  wait_gateway_up(fx, g);
  std::string order;
  const Qty filled = fill_one_of(fx, ea, &order);
  REQUIRE_MESSAGE(filled.is_positive(), fastmm::test::read_file(g.log));
  fx.server.set_open_orders_delay_ms(0);
  // The account holds it, for nobody.
  std::optional<StatusPosition> btc;
  const bool nobodys = wait_until(
      [&] {
        btc = gateway_position(gw_name);
        return btc && btc->qty_raw == position(fx.server.stats(), 0).raw && btc->qty_raw != 0 &&
               std::abs(btc->unattributed_raw) == filled.raw && btc->unexplained_raw == 0;
      },
      10000);
  REQUIRE(btc.has_value());
  INFO("gateway BTCUSDT " << btc->qty_raw << " unattributed " << btc->unattributed_raw
                          << " unexplained " << btc->unexplained_raw << "; filled " << filled.raw);
  REQUIRE_MESSAGE(nobodys, fastmm::test::read_file(g.log));
  // The sweep, answered, has cancelled a's other orders.
  REQUIRE_MESSAGE(wait_until([&] { return fx.server.stats().open_orders == 0; }, 20000),
                  fastmm::test::read_file(g.log));

  // a attaches: the fill is a's, in its store and in the account's share.
  const pid_t a2 = spawn_strategy(c.a, g);
  const std::uint16_t ea2 = wait_resting(fx, c.a, {ea});
  CHECK(ea2 != ea);
  CHECK_MESSAGE(wait_until([&] { return stored_holds(c.a, c.a_name, order); }, 10000),
                "a never booked " << order << ": " << fastmm::test::read_file(c.a.config + ".log")
                                  << fastmm::test::read_file(g.log));
  const bool claimed = wait_until(
      [&] {
        btc = gateway_position(gw_name);
        return btc && btc->qty_raw == position(fx.server.stats(), 0).raw &&
               btc->unattributed_raw == 0 && btc->unexplained_raw == 0;
      },
      10000);
  INFO("gateway BTCUSDT " << btc->qty_raw << " unattributed " << btc->unattributed_raw
                          << " unexplained " << btc->unexplained_raw);
  CHECK(claimed);

  stop_strategy(a2);
  CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
  stop_gateway(g);
  const StoredFills fa = stored(c.a, c.a_name);
  CHECK(std::count(fa.cl_ord_ids.begin(), fa.cl_ord_ids.end(), order) == 1);
  CHECK(booked_twice(c.a, c.a_name).empty());
  const Qty pa = store_position(c.a, c.a_name);
  CHECK(pa == fa.sum);
  CHECK(pa == position(fx.server.stats(), 0));
  CHECK(logged_share(g, c.a_name) == pa);
  CHECK(logged_share(g, "unattributed") == Qty{});
}

#endif  // FASTMM_LIVE_EXE && FASTMM_GATEWAY_EXE
