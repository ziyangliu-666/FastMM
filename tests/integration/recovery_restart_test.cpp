// A whole fastmm-live process is killed with SIGKILL while it has orders resting, and the next one
// has to take over: a fresh client-order-id epoch so no id repeats, the orders the dead session
// left behind cleared rather than abandoned, the loss budget and a latched max-loss trip carried
// across, and no order placed twice. The simulator stays up in this process; the sessions are real
// child processes, because a crash has to be a crash.
#include "process_util.hpp"

#include "fastmm/core/session_state.hpp"
#include "fastmm/live/session.hpp"
#include "fastmm/net/crypto.hpp"
#include "fastmm/store/reader.hpp"
#include "fastmm/store/registry.hpp"
#include "fastmm/venues/blocking_http.hpp"

#include <algorithm>
#include <csignal>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

namespace {

#ifdef FASTMM_LIVE_EXE

KillState load_kill(const std::string& path) {
  auto st = KillStateStore::load(path);
  REQUIRE_MESSAGE(st.has_value(), (st ? std::string() : st.error()));
  return *st;
}

// An order resting on the account with a client order id from session epoch 1 and nobody to manage
// it: what a session that died without cancelling leaves behind. Placed over signed REST after the
// crash rather than relying on the dead session's quotes, since the counter-party flow can take any
// resting order off the book. Each attempt uses a fresh id, so a retry is never a repeated one.
// Returns the id of the order that is still resting when it returns.
std::string place_orphan(const ServerFixture& fx) {
  std::string resting;
  const bool placed = wait_until(
      [&, attempt = std::uint64_t{0}]() mutable {
        const std::string id(encode_cl_ord_id(cid(9900 + attempt++)).view());
        const std::string query =
            "symbol=BTCUSDT&side=BUY&type=LIMIT_MAKER&quantity=0.001&price=50000.00"
            "&newClientOrderId=" +
            id + "&recvWindow=5000&timestamp=" + std::to_string(fx.server.server_time_ms());
        venues::BlockingHttp http(fx.http());
        const venues::HttpReply r = http.request(
            "POST",
            "/api/v3/order?" + query +
                "&signature=" + std::string(net::hmac_sha256_hex(kApiSecret, query).view()),
            std::string("X-MBX-APIKEY: ") + kApiKey + "\r\n");
        REQUIRE_MESSAGE(r.status == 200, r.body);
        const std::vector<std::string> open = fx.server.open_client_order_ids();
        if (std::find(open.begin(), open.end(), id) == open.end()) return false;
        resting = id;
        return true;
      },
      10000);
  REQUIRE_MESSAGE(placed, "no order stayed on the book long enough to be orphaned");
  return resting;
}

#endif  // FASTMM_LIVE_EXE

}  // namespace

#ifdef FASTMM_LIVE_EXE

TEST_CASE("recovery: a SIGKILLed session leaves orders behind and the next one clears them") {
  ServerFixture fx;
  const SessionFiles f = write_config(fx, "recovery-crash", "exit", "50");
  remove_all_of({f.epoch, f.kill, f.journal_dir});

  // First session: run until it has quotes resting at the venue, then kill it outright.
  const pid_t first = spawn_live(f, 120);
  // Let it trade first: the loss budget it spends has to be what the next session starts from.
  REQUIRE_MESSAGE(wait_until([&] { return fx.server.stats().fills >= 1; }, 60000),
                  "the first session never traded");
  REQUIRE(wait_until(
      [&] {
        auto st = KillStateStore::load(f.kill);
        return st && st->fees.is_positive();
      },
      20000));
  REQUIRE(::kill(first, SIGKILL) == 0);
  CHECK(reap(first) == -1);  // signalled, not exited: no shutdown ran

  // No shutdown ran, so nothing was cancelled: whatever the session was quoting is still resting.
  MESSAGE("the crash left " << fx.server.open_client_order_ids().size() << " order(s) resting");
  const std::string orphan = place_orphan(fx);
  const std::uint64_t accepted_before = fx.server.stats().orders_accepted;
  std::vector<std::string> orphans = fx.server.open_client_order_ids();
  if (std::find(orphans.begin(), orphans.end(), orphan) == orphans.end()) orphans.push_back(orphan);
  const KillState after_crash = load_kill(f.kill);
  CHECK(after_crash.sessions == 1);
  CHECK_FALSE(after_crash.latched);
  CHECK(after_crash.fees.is_positive());

  // Second session on the same state files.
  const pid_t second = spawn_live(f, 120);
  REQUIRE_MESSAGE(
      wait_until([&] { return fx.server.stats().orders_accepted > accepted_before; }, 60000),
      "the second session never placed an order");
  // The orders of the dead session are gone while the new one is still running: it found them in
  // the open-orders snapshot, did not recognise the ids and cancelled them.
  const bool cleared = wait_until(
      [&] {
        const std::vector<std::string> open = fx.server.open_client_order_ids();
        for (const std::string& id : orphans) {
          if (std::find(open.begin(), open.end(), id) != open.end()) return false;
        }
        return true;
      },
      30000);
  CHECK_MESSAGE(cleared, "the second session left the crashed session's orders resting");

  REQUIRE(::kill(second, SIGTERM) == 0);  // the normal shutdown: cancel all, then stop
  CHECK(reap(second) == live::kExitOk);
  REQUIRE(wait_until([&] { return fx.server.stats().open_orders == 0; }, 10000));

  // A fresh epoch means no client order id can repeat, and the loss budget carried over.
  const sim::server::SimServerStats ss = fx.server.stats();
  CHECK(ss.duplicate_client_order_ids == 0);
  const KillState after_second = load_kill(f.kill);
  CHECK(after_second.sessions == 2);
  CHECK(after_second.fees.raw >= after_crash.fees.raw);  // the budget carried, it did not reset
  MESSAGE("venue: accepted=" << ss.orders_accepted << " fills=" << ss.fills << " orphans="
                             << orphans.size() << " carry=" << after_second.carry().raw);
}

TEST_CASE("recovery: a latched max-loss trip survives a SIGKILL and the next start refuses") {
  // Commissions far above BasicMM's 5 bps half spread: every fill loses money.
  sim::server::SimServerConfig sc = test_server_config();
  sc.maker_bps = 50.0;
  sc.taker_bps = 50.0;
  ServerFixture fx(std::move(sc));
  // on_kill = "stay": the session keeps running once latched, so the kill can be a crash.
  const SessionFiles f = write_config(fx, "recovery-latch", "stay", "0.01");
  remove_all_of({f.epoch, f.kill, f.journal_dir});

  const pid_t first = spawn_live(f, 120);
  const bool latched = wait_until(
      [&] {
        if (!std::filesystem::exists(f.kill)) return false;
        auto st = KillStateStore::load(f.kill);
        return st && st->latched;
      },
      90000);
  REQUIRE(::kill(first, SIGKILL) == 0);
  CHECK(reap(first) == -1);
  REQUIRE_MESSAGE(latched, "the first session never latched a max-loss trip");
  CHECK(load_kill(f.kill).reason == KillReason::MaxLoss);

  // The next start refuses before it trades.
  const std::uint64_t accepted = fx.server.stats().orders_accepted;
  const pid_t second = spawn_live(f, 30);
  CHECK(reap(second) == live::kExitKilled);
  CHECK(fx.server.stats().orders_accepted == accepted);

  // Cleaning the state arms the budget again; the session then runs and latches once more.
  REQUIRE(KillStateStore::clear(f.kill).has_value());
  const pid_t third = spawn_live(f, 120);
  REQUIRE(wait_until([&] { return fx.server.stats().orders_accepted > accepted; }, 60000));
  REQUIRE(::kill(third, SIGTERM) == 0);
  static_cast<void>(reap(third));
  CHECK(fx.server.stats().duplicate_client_order_ids == 0);
}

// A restarted session has to pick its position up where the last one left it, and account for what
// happened while nothing was running. The first session trades and stops; a trade is then made on
// the account outside FastMM; the second session starts from the store's position, replays the
// venue's executions since the store's last fill - which names the outside trade, not the ones the
// store already has - and ends holding exactly what the venue holds.
TEST_CASE(
    "recovery: a restart carries the position over and books what happened while it was down") {
  ServerFixture fx(test_server_config());
  const SessionFiles f = write_config(fx, "recovery-position", "exit", "1000");
  remove_all_of({f.epoch, f.kill, f.journal_dir, f.config + ".log"});

  const pid_t first = spawn_live(f, 60);
  REQUIRE(wait_until([&] { return !fx.server.stats().position.is_zero(); }, 45000));
  REQUIRE(::kill(first, SIGTERM) == 0);
  CHECK(reap(first) == 0);
  const Qty after_first = fx.server.stats().position;
  REQUIRE_FALSE(after_first.is_zero());

  // A trade nobody running made: a market order over signed REST.
  {
    const std::string query =
        "symbol=BTCUSDT&side=BUY&type=MARKET&quantity=0.002&recvWindow=5000&timestamp=" +
        std::to_string(fx.server.server_time_ms());
    venues::BlockingHttp http(fx.http());
    const venues::HttpReply r = http.request(
        "POST",
        "/api/v3/order?" + query +
            "&signature=" + std::string(net::hmac_sha256_hex(kApiSecret, query).view()),
        std::string("X-MBX-APIKEY: ") + kApiKey + "\r\n");
    REQUIRE_MESSAGE(r.status == 200, r.body);
  }
  const Qty before_second = fx.server.stats().position;
  REQUIRE(before_second != after_first);

  // The second session: it must start from before_second, not from zero and not from after_first.
  const pid_t second = spawn_live(f, 60);
  const std::string log = f.config + ".log";
  REQUIRE(wait_until(
      [&] {
        return fastmm::test::read_file(log).find("restored position BTCUSDT") != std::string::npos;
      },
      30000));
  // Let it quote and trade a little, then stop it; its final position is what the store records.
  REQUIRE(wait_until([&] { return fx.server.stats().orders_accepted > 0; }, 30000));
  std::this_thread::sleep_for(std::chrono::seconds(3));
  REQUIRE(::kill(second, SIGTERM) == 0);
  CHECK(reap(second) == 0);

  store::register_builtin_backends();
  auto reader = store::StoreRegistry::instance().make_reader("sqlite");
  REQUIRE(reader != nullptr);
  store::BackendOptions opts;
  opts.engine_name = "recovery-position";
  opts.default_dir = f.journal_dir;
  opts.read_only = true;
  REQUIRE(reader->open(opts).has_value());
  store::QueryFilter qf;
  qf.engine = "recovery-position";
  auto rec = reader->recovery(qf);
  REQUIRE(rec.has_value());
  REQUIRE(rec->found);
  Qty engine_view{};
  for (const store::Recovery::PositionState& p : rec->position_state) {
    if (p.symbol == "BTCUSDT") engine_view = Qty::from_raw(p.qty_raw);
  }
  INFO("venue " << fx.server.stats().position.raw << ", engine " << engine_view.raw
                << ", after the first session " << after_first.raw << ", before the second "
                << before_second.raw);
  CHECK(engine_view == fx.server.stats().position);
  CHECK(fx.server.stats().duplicate_client_order_ids == 0);
}

#endif  // FASTMM_LIVE_EXE
