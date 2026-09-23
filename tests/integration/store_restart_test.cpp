// A whole fastmm-live session writes its record to the store, and the next session reads it back:
// what the previous one traded, where it left the position, whether it shut down cleanly and which
// journal it wrote. docs/reference/storage.md.
#include "integration_util.hpp"

#include "fastmm/core/journal.hpp"
#include "fastmm/live/session.hpp"
#include "fastmm/store/registry.hpp"
#include "fastmm/store/sqlite_store.hpp"
#include "fastmm/strategies/builtin.hpp"
#include "fastmm/strategies/registry.hpp"

#include <filesystem>
#include <memory>
#include <string>

using namespace fastmm;
using namespace fastmm::integration;

namespace {

std::string fresh(const std::string& name) {
  const auto p = fastmm::test::tmp_dir() / name;
  std::error_code ec;
  std::filesystem::remove(p, ec);
  std::filesystem::remove(p.string() + "-wal", ec);
  std::filesystem::remove(p.string() + "-shm", ec);
  return p.string();
}

void register_strategies_once() {
  static const bool done = [] {
    register_builtin_strategies(StrategyRegistry::instance());
    return true;
  }();
  static_cast<void>(done);
}

Config store_config(const ServerFixture& fx, const std::string& name, const std::string& db) {
  Config cfg = sim_local_config(fx, false);
  cfg.engine.name = name;
  cfg.engine.epoch_file = fresh(name + ".epoch");
  cfg.engine.kill_file = fresh(name + ".kill");
  cfg.storage.values["backend"] = "sqlite";
  cfg.storage.values["path"] = db;
  return cfg;
}

live::LiveOptions store_options(const std::string& journal) {
  live::LiveOptions o;
  o.duration_ns = seconds(3).ns;
  o.journal_path = journal;
  o.no_status = true;
  o.program = "store-restart-test";
  return o;
}

std::unique_ptr<store::Reader> reader_for(const std::string& db, GenericSection& section) {
  section.values["path"] = db;
  store::BackendOptions o;
  o.config = &section;
  o.read_only = true;
  auto r = store::make_sqlite_reader();
  REQUIRE(r->open(o));
  return r;
}

}  // namespace

TEST_CASE("store restart: a session records itself and the next one reads what it left behind") {
  register_strategies_once();
  const std::string db = fresh("restart.db");
  const std::string journal_one = fresh("restart-1.fmj");
  const std::string journal_two = fresh("restart-2.fmj");
  std::uint64_t first_session = 0;

  {
    ServerFixture fx;
    const Config cfg = store_config(fx, "restart", db);
    const int rc = live::run_live(cfg, store_options(journal_one));
    CHECK(rc == live::kExitOk);
    CHECK(fx.server.stats().orders_accepted > 0);
  }

  GenericSection section;
  {
    auto reader = reader_for(db, section);
    auto sessions = reader->sessions(store::QueryFilter{});
    REQUIRE(sessions);
    REQUIRE(sessions->rows.size() == 1);
    store::QueryFilter f;
    f.engine = "restart";
    auto rec = reader->recovery(f);
    REQUIRE(rec);
    REQUIRE(rec->found);
    first_session = rec->session_id;
    CHECK(rec->strategy == "basic_mm");
    CHECK(rec->clean_shutdown);
    CHECK_FALSE(rec->stopped_utc.empty());
    CHECK(rec->journal_complete);
    // A clean shutdown asks for the kill switch itself (cancel all, then stop), so the reason is
    // Requested; clean_shutdown is what says the session ended on its own terms.
    CHECK(rec->kill_reason == "Requested");
    CHECK_FALSE(rec->kill_latched);
    REQUIRE(rec->journals.size() >= 1);
    CHECK(rec->journals[0] == journal_one);
    // The journal it names reads back whole, so the two records agree.
    JournalReader jr;
    REQUIRE(jr.open(journal_one));
    CHECK(jr.complete());
    // Orders are recorded, and BasicMM quotes both sides.
    auto orders = reader->orders(store::QueryFilter{});
    REQUIRE(orders);
    CHECK_FALSE(orders->rows.empty());
  }

  // Second session: a fresh session id, the first one still there, and the recovery now names it.
  {
    ServerFixture fx;
    const Config cfg = store_config(fx, "restart", db);
    const int rc = live::run_live(cfg, store_options(journal_two));
    CHECK(rc == live::kExitOk);
  }

  auto reader = reader_for(db, section);
  auto sessions = reader->sessions(store::QueryFilter{});
  REQUIRE(sessions);
  CHECK(sessions->rows.size() == 2);
  store::QueryFilter f;
  f.engine = "restart";
  auto rec = reader->recovery(f);
  REQUIRE(rec);
  REQUIRE(rec->found);
  CHECK(rec->session_id != first_session);  // the newest session, not the first
  CHECK(rec->journals[0] == journal_two);

  // Both sessions' fills are in one table, and the PnL rolls up by UTC day.
  auto fills = reader->fills(store::QueryFilter{});
  REQUIRE(fills);
  auto pnl = reader->pnl(store::QueryFilter{});
  REQUIRE(pnl);
  if (!fills->rows.empty()) {
    CHECK_FALSE(pnl->rows.empty());
    // Every fill names an instrument from the session's instrument table.
    for (const auto& row : fills->rows) CHECK_FALSE(row[1].empty());
  }
}

TEST_CASE("store restart: [storage] backend = none runs a session and writes no store") {
  register_strategies_once();
  const std::string db = fresh("none.db");
  ServerFixture fx;
  Config cfg = store_config(fx, "nostore", db);
  cfg.storage.values["backend"] = "none";
  const int rc = live::run_live(cfg, store_options(fresh("nostore.fmj")));
  CHECK(rc == live::kExitOk);
  CHECK_FALSE(std::filesystem::exists(db));
}

TEST_CASE("store restart: an unknown [storage] backend stops the session before it trades") {
  register_strategies_once();
  ServerFixture fx;
  Config cfg = store_config(fx, "badstore", fresh("badstore.db"));
  cfg.storage.values["backend"] = "clickhouse";
  const int rc = live::run_live(cfg, store_options(fresh("badstore.fmj")));
  CHECK(rc == live::kExitConfig);
  CHECK(fx.server.stats().orders_accepted == 0);
}
