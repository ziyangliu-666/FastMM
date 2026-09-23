// Schema versioning: a store created at an older version is migrated in place, and one written by
// a newer FastMM is refused rather than corrupted.
#include "../../src/store/sqlite_schema.hpp"
#include "store_test_util.hpp"

#include "fastmm/store/sqlite_store.hpp"

#include <sqlite3.h>

#include <filesystem>
#include <string>

using namespace fastmm;
using namespace fastmm::store;
using fastmm::test::kDay1Ns;
using fastmm::test::kDay2Ns;
using fastmm::test::tmp_dir;

namespace {

struct Db {
  sqlite3* db = nullptr;
  explicit Db(const std::string& path) {
    REQUIRE(
        sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) ==
        SQLITE_OK);
  }
  ~Db() {
    if (db != nullptr) static_cast<void>(sqlite3_close_v2(db));
  }
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;
};

std::string fresh(const char* name) {
  const auto p = tmp_dir() / name;
  std::error_code ec;
  std::filesystem::remove(p, ec);
  std::filesystem::remove(p.string() + "-wal", ec);
  std::filesystem::remove(p.string() + "-shm", ec);
  return p.string();
}

bool has_table(sqlite3* db, const char* name) {
  sqlite3_stmt* st = nullptr;
  REQUIRE(sqlite3_prepare_v2(
              db, "SELECT count(*) FROM sqlite_master WHERE name = ?", -1, &st, nullptr) ==
          SQLITE_OK);
  sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
  const bool found = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) > 0;
  sqlite3_finalize(st);
  return found;
}

std::int64_t scalar(sqlite3* db, const char* sql) {
  sqlite3_stmt* st = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
  const std::int64_t v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
  sqlite3_finalize(st);
  return v;
}

}  // namespace

TEST_CASE("store.migration: an empty database is created at the newest version") {
  const std::string path = fresh("migrate_new.db");
  Db d(path);
  CHECK(*sqlite::schema_version(d.db) == 0);
  auto v = sqlite::migrate(d.db);
  REQUIRE(v);
  CHECK(*v == kSqliteSchemaVersion);
  CHECK(has_table(d.db, "sessions"));
  CHECK(has_table(d.db, "pnl_daily"));
  CHECK(has_table(d.db, "pnl_by_day"));
  // Migrating again is a no-op.
  auto again = sqlite::migrate(d.db);
  REQUIRE(again);
  CHECK(*again == kSqliteSchemaVersion);
}

TEST_CASE("store.migration: a version 1 store gains the PnL roll-up and is backfilled") {
  const std::string path = fresh("migrate_v1.db");
  {
    Db d(path);
    auto v = sqlite::migrate(d.db, 1);
    REQUIRE(v);
    CHECK(*v == 1);
    CHECK(has_table(d.db, "positions"));
    CHECK_FALSE(has_table(d.db, "pnl_daily"));
    // Two days of position snapshots recorded under version 1, cumulative within the session.
    REQUIRE(sqlite::exec(d.db,
                         "INSERT INTO sessions (session_id, engine, strategy, session_epoch,"
                         " started_ns, started_day, version, build, config_hash, dry_run,"
                         " pnl_carry_raw) VALUES (1,'test','basic_mm',1,0,'2024-03-04','0.1.0',"
                         "'b','0',0,0)"));
    REQUIRE(sqlite::exec(d.db,
                         "INSERT INTO instruments VALUES "
                         "(1,0,0,'BTCUSDT','BTC','USDT','USDT','Spot',0,1,1,100000000)"));
    const std::string rows =
        "INSERT INTO positions VALUES"
        " (1,1," +
        std::to_string(kDay1Ns) +
        ",'2024-03-04',0,'BTCUSDT',0,0,100,0,10,5,2,100,0,10,0),"
        " (1,2," +
        std::to_string(kDay1Ns + 60) +
        ",'2024-03-04',0,'BTCUSDT',0,0,300,0,30,9,4,300,"
        "0,30,0),"
        " (1,3," +
        std::to_string(kDay2Ns) +
        ",'2024-03-05',0,'BTCUSDT',5,0,500,7,50,15,6,500,7,50,"
        "0)";
    REQUIRE(sqlite::exec(d.db, rows.c_str()));
  }
  Db d(path);
  auto v = sqlite::migrate(d.db);
  REQUIRE(v);
  CHECK(*v == kSqliteSchemaVersion);
  REQUIRE(has_table(d.db, "pnl_daily"));
  CHECK(scalar(d.db, "SELECT count(*) FROM pnl_daily") == 2);
  // Day one takes the last snapshot of that day; day two takes the change since it.
  CHECK(scalar(d.db, "SELECT realized_raw FROM pnl_daily WHERE day='2024-03-04'") == 300);
  CHECK(scalar(d.db, "SELECT fees_raw FROM pnl_daily WHERE day='2024-03-04'") == 30);
  CHECK(scalar(d.db, "SELECT fills FROM pnl_daily WHERE day='2024-03-04'") == 4);
  CHECK(scalar(d.db, "SELECT realized_raw FROM pnl_daily WHERE day='2024-03-05'") == 200);
  CHECK(scalar(d.db, "SELECT fees_raw FROM pnl_daily WHERE day='2024-03-05'") == 20);
  CHECK(scalar(d.db, "SELECT fills FROM pnl_daily WHERE day='2024-03-05'") == 2);
  CHECK(scalar(d.db, "SELECT unrealized_raw FROM pnl_daily WHERE day='2024-03-05'") == 7);
  // The settlement currency comes from the instrument table of that session.
  sqlite3_stmt* st = nullptr;
  REQUIRE(sqlite3_prepare_v2(
              d.db, "SELECT settlement_ccy FROM pnl_daily LIMIT 1", -1, &st, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(st) == SQLITE_ROW);
  CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))) == "USDT");
  sqlite3_finalize(st);
}

TEST_CASE("store.migration: a store from a newer FastMM is refused") {
  const std::string path = fresh("migrate_future.db");
  {
    Db d(path);
    REQUIRE(sqlite::migrate(d.db));
    const std::string sql =
        "UPDATE schema_version SET version = " + std::to_string(kSqliteSchemaVersion + 5);
    REQUIRE(sqlite::exec(d.db, sql.c_str()));
  }
  Db d(path);
  auto v = sqlite::migrate(d.db);
  REQUIRE_FALSE(v);
  CHECK(v.error().find("newer FastMM") != std::string::npos);
}

TEST_CASE("store.migration: the reader refuses a file that is not a store") {
  const std::string path = fresh("not_a_store.db");
  {
    Db d(path);
    REQUIRE(sqlite::exec(d.db, "CREATE TABLE junk (x INTEGER)"));
  }
  GenericSection s;
  s.values["path"] = path;
  BackendOptions o;
  o.config = &s;
  o.read_only = true;
  auto reader = make_sqlite_reader();
  auto r = reader->open(o);
  REQUIRE_FALSE(r);
  CHECK(r.error().find("not a FastMM store") != std::string::npos);
}

TEST_CASE("store.migration: every step raises the recorded version by one") {
  const auto steps = sqlite::migrations();
  REQUIRE_FALSE(steps.empty());
  CHECK(steps.front().version == 1);
  CHECK(steps.back().version == kSqliteSchemaVersion);
  for (std::size_t i = 1; i < steps.size(); ++i)
    CHECK(steps[i].version == steps[i - 1].version + 1);
}

TEST_CASE("store.migration: UTC day and stamp helpers") {
  CHECK(sqlite::utc_day(kDay1Ns) == "2024-03-04");
  CHECK(sqlite::utc_day(kDay1Ns + 86'399'999'999'999LL) == "2024-03-04");
  CHECK(sqlite::utc_day(kDay2Ns) == "2024-03-05");
  CHECK(sqlite::utc_day(0) == "1970-01-01");
  CHECK(sqlite::utc_stamp(kDay1Ns + 3'661'000'000'000LL) == "2024-03-04 01:01:01");
  CHECK(sqlite::utc_stamp(0).empty());
  std::int64_t first = 0;
  std::int64_t last = 0;
  REQUIRE(sqlite::day_bounds("2024-03-04", first, last));
  CHECK(first == kDay1Ns);
  CHECK(last == kDay2Ns - 1);
  CHECK_FALSE(sqlite::day_bounds("not-a-day", first, last));
  CHECK(sqlite::decimal(150'000'000) == "1.5");
  CHECK(sqlite::decimal(-1) == "-0.00000001");
}
