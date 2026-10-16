// What a restarted fastmm-live session carries over, checked against the store before it trades
// (live/restart_check.hpp):
//   * the epoch file gone while the store holds sessions: exit 9, nothing sent; the counter written
//     back as the message says, the session runs on a later epoch;
//   * the kill file gone after a max-loss trip the store recorded: exit 6; --clear-kill runs;
//   * a store whose newest session booked an execution twice, on a venue that reports no position
//     of its own: exit 9; --accept-position or restore_position = false runs;
#include "../store/store_test_util.hpp"
#include "integration_util.hpp"

#include "fastmm/core/session_state.hpp"
#include "fastmm/live/restart_check.hpp"
#include "fastmm/live/session.hpp"
#include "fastmm/store/registry.hpp"
#include "fastmm/store/sqlite_store.hpp"
#include "fastmm/strategies/builtin.hpp"
#include "fastmm/strategies/registry.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace fastmm;
using namespace fastmm::integration;

namespace {

std::string fresh(const std::string& name) {
  const auto p = fastmm::test::tmp_dir() / name;
  std::error_code ec;
  std::filesystem::remove(p, ec);
  return p.string();
}

void register_strategies_once() {
  static const bool done = [] {
    register_builtin_strategies(StrategyRegistry::instance());
    return true;
  }();
  static_cast<void>(done);
}

Config state_config(const ServerFixture& fx, const std::string& name) {
  Config cfg = sim_local_config(fx, false);
  cfg.engine.name = name;
  cfg.engine.epoch_file = (fastmm::test::tmp_dir() / (name + ".epoch")).string();
  cfg.engine.kill_file = (fastmm::test::tmp_dir() / (name + ".kill")).string();
  cfg.storage.values["backend"] = "sqlite";
  cfg.storage.values["path"] = (fastmm::test::tmp_dir() / (name + ".db")).string();
  return cfg;
}

// Removes the store and the state files of `name`: the deployment's first start.
void first_start(const std::string& name) {
  static_cast<void>(fresh_store(name + ".db"));
  static_cast<void>(fresh(name + ".epoch"));
  static_cast<void>(fresh(name + ".kill"));
}

live::LiveOptions state_options(const std::string& name, std::int64_t seconds_to_run = 3) {
  live::LiveOptions o;
  o.duration_ns = seconds(seconds_to_run).ns;
  o.no_journal = true;
  o.status_path = fresh(name + ".status");
  o.program = "restart-state-test";
  return o;
}

store::Recovery recovery_of(const Config& cfg) {
  auto reader = store::make_sqlite_reader();
  store::BackendOptions o;
  o.config = &cfg.storage;
  o.read_only = true;
  REQUIRE(reader->open(o));
  store::QueryFilter f;
  f.engine = cfg.engine.name;
  auto r = reader->recovery(f);
  REQUIRE(r);
  return *r;
}

// Adds a session of `cfg`'s engine on venue "sim" to its store, started `minute` minutes into
// kDay1Ns and written as `body` says.
template <class Body>
void seed_store(const Config& cfg, std::uint64_t session, std::int64_t minute, Body&& body) {
  auto backend = store::make_sqlite_backend();
  store::BackendOptions o;
  o.config = &cfg.storage;
  o.engine_name = cfg.engine.name;
  REQUIRE(backend->open(o));
  store::SessionOpen s =
      fastmm::test::store_session(session, fastmm::test::kDay1Ns + minute * 60'000'000'000);
  s.engine_name = cfg.engine.name;
  s.venues = {"sim"};
  REQUIRE(backend->session_open(s));
  const InstrumentTable t = fastmm::test::store_table();
  REQUIRE(backend->instruments(session, std::span<const Instrument>(t.data(), t.size())));
  backend->begin();
  body(*backend);
  backend->commit();
  backend->close();
}

}  // namespace

TEST_CASE("restart state: a lost epoch file is refused while the store holds sessions") {
  register_strategies_once();
  first_start("state-epoch");
  ServerFixture fx;
  const Config cfg = state_config(fx, "state-epoch");
  REQUIRE(live::run_live(cfg, state_options("state-epoch")) == live::kExitOk);
  const store::Recovery first = recovery_of(cfg);
  REQUIRE(first.found);
  REQUIRE_FALSE(first.session_epochs.empty());
  const std::uint16_t first_epoch = first.session_epochs.front();

  std::filesystem::remove(cfg.engine.epoch_file);
  const std::uint64_t accepted = fx.server.stats().orders_accepted;
  CHECK(live::run_live(cfg, state_options("state-epoch")) == live::kExitState);
  CHECK(fx.server.stats().orders_accepted == accepted);
  CHECK_FALSE(std::filesystem::exists(cfg.engine.epoch_file));  // no counter started at 1
  CHECK(recovery_of(cfg).session_id == first.session_id);       // no session recorded

  // The fix the refusal names: the newest epoch written back.
  const live::RestartRefusal r = live::check_epoch_file(first, cfg.engine.epoch_file);
  CHECK(r.exit_code == live::kExitState);
  CHECK(r.message.find("echo " + std::to_string(first_epoch) + " > " + cfg.engine.epoch_file) !=
        std::string::npos);
  {
    std::ofstream out(cfg.engine.epoch_file);
    out << first_epoch << "\n";
  }
  REQUIRE(live::run_live(cfg, state_options("state-epoch")) == live::kExitOk);
  const store::Recovery last = recovery_of(cfg);
  CHECK(last.session_id != first.session_id);
  REQUIRE_FALSE(last.session_epochs.empty());
  CHECK(last.session_epochs.front() == first_epoch + 1);
  CHECK(fx.server.stats().duplicate_client_order_ids == 0);
}

TEST_CASE("restart state: a lost kill file does not clear a max-loss trip the store recorded") {
  register_strategies_once();
  first_start("state-kill");
  ServerFixture fx;
  const Config cfg = state_config(fx, "state-kill");
  // The newest session tripped max_loss and was killed before it recorded a shutdown; the kill
  // file that latched it is gone, the epoch file is not.
  seed_store(cfg, 11, 0, [](store::Backend& b) {
    b.kill(fastmm::test::store_kill(11, 1, fastmm::test::kDay1Ns + 1, KillReason::MaxLoss));
  });
  {
    std::ofstream out(cfg.engine.epoch_file);
    out << "7\n";
  }
  CHECK(live::run_live(cfg, state_options("state-kill")) == live::kExitKilled);
  CHECK(fx.server.stats().orders_accepted == 0);

  live::LiveOptions clear = state_options("state-kill");
  clear.clear_kill = true;
  CHECK(live::run_live(cfg, clear) == live::kExitOk);
  CHECK(fx.server.stats().orders_accepted > 0);
  // The cleared session is the newest now: the next start needs nothing.
  CHECK(live::run_live(cfg, state_options("state-kill")) == live::kExitOk);
}

TEST_CASE("restart state: a position the newest session booked twice is not traded on") {
  register_strategies_once();
  first_start("state-position");
  ServerFixture fx;
  Config cfg = state_config(fx, "state-position");
  // Execution "501" booked by one session and again by the next, whose replay did not know it had
  // it: the position the newest left, 0.002, holds 0.001 twice.
  const auto buy_501 = [](std::uint64_t session, std::int64_t position_raw) {
    return [session, position_raw](store::Backend& b) {
      FillRecord f = fastmm::test::store_fill(
          session, 1, fastmm::test::kDay1Ns, Side::Buy, 5'000'000'000'000, 100'000, 0, 0, "501");
      f.hdr.venue = VenueId{0};
      f.position_qty = Qty::from_raw(position_raw);
      b.fill(f);
      PositionRecord p = fastmm::test::store_position(
          session, 2, fastmm::test::kDay1Ns + 1, position_raw, 0, 0, 1);
      p.hdr.venue = VenueId{0};
      b.position(p);
    };
  };
  seed_store(cfg, 20, 0, buy_501(20, 100'000));
  seed_store(cfg, 21, 1, buy_501(21, 200'000));
  {
    std::ofstream out(cfg.engine.epoch_file);
    out << "7\n";
  }
  const store::Recovery seeded = recovery_of(cfg);
  REQUIRE(seeded.double_booked.size() == 1);
  CHECK(seeded.double_booked[0].venue == "sim");
  CHECK(seeded.double_booked[0].symbol == "BTCUSDT");

  CHECK(live::run_live(cfg, state_options("state-position")) == live::kExitState);
  CHECK(fx.server.stats().orders_accepted == 0);

  SUBCASE("--accept-position trades from the stored position") {
    live::LiveOptions o = state_options("state-position");
    o.accept_position = true;
    CHECK(live::run_live(cfg, o) == live::kExitOk);
    CHECK(fx.server.stats().orders_accepted > 0);
    // Its own session is the newest now and booked nothing twice.
    CHECK(recovery_of(cfg).double_booked.empty());
    CHECK(live::run_live(cfg, state_options("state-position")) == live::kExitOk);
  }
  SUBCASE("restore_position = false starts flat") {
    cfg.engine.restore_position = false;
    CHECK(live::run_live(cfg, state_options("state-position")) == live::kExitOk);
    CHECK(fx.server.stats().orders_accepted > 0);
  }
}

TEST_CASE("restart state: a venue that reports its own position replaces a double-booked one") {
  store::Recovery prev;
  prev.found = true;
  prev.session_id = 3;
  prev.double_booked = {{"perps", "BTCUSDT", 2}, {"spot", "ETHUSDT", 1}, {"#0", "SOLUSDT", 1}};
  const live::RestoredInstrument restored[] = {{"perps", "BTCUSDT", /*venue_reports=*/true},
                                               {"spot", "ETHUSDT", false},
                                               {"spot", "SOLUSDT", false},
                                               {"spot", "XRPUSDT", false}};
  const auto wrong = live::wrong_positions(prev, restored);
  REQUIRE(wrong.size() == 2);
  CHECK(wrong[0].symbol == "ETHUSDT");
  CHECK(wrong[1].symbol == "SOLUSDT");  // a store without venue names matches by symbol
  const live::RestartRefusal r = live::position_refusal(prev, wrong, "mm1");
  CHECK(r.exit_code == live::kExitState);
  CHECK(r.message.find("1 spot ETHUSDT, 1 #0 SOLUSDT") != std::string::npos);
  CHECK(r.message.find("fastmm-pnl duplicates --engine mm1") != std::string::npos);
  CHECK(r.message.find("--accept-position") != std::string::npos);
  // Not restored at all (the venue cannot replay): nothing to refuse.
  CHECK(live::wrong_positions(prev, {}).empty());
}
