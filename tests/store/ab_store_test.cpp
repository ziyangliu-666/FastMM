// [ab] in the SQLite store (schema 12): the session's arms, the arm of each fill, order and
// parameter update, and the copy an older store gets before it is migrated.
#include "../../src/store/sqlite_schema.hpp"
#include "store_test_util.hpp"

#include "fastmm/store/sqlite_store.hpp"

#include <sqlite3.h>

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

using namespace fastmm;
using namespace fastmm::store;
using fastmm::test::kDay1Ns;
using fastmm::test::tmp_dir;

namespace {

std::string fresh(const char* name) {
  const auto p = tmp_dir() / name;
  std::error_code ec;
  for (const char* suffix : {"", "-wal", "-shm", ".v11.bak"})
    std::filesystem::remove(p.string() + suffix, ec);
  return p.string();
}

std::string text(const std::string& path, const char* sql) {
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
  sqlite3_stmt* st = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
  std::string v = "<none>";
  if (sqlite3_step(st) == SQLITE_ROW) {
    const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    v = p == nullptr ? "<null>" : p;
  }
  sqlite3_finalize(st);
  sqlite3_close_v2(db);
  return v;
}

std::unique_ptr<Backend> open_backend(const std::string& path, GenericSection& section) {
  section.values["path"] = path;
  BackendOptions o;
  o.config = &section;
  o.engine_name = "test";
  auto b = make_sqlite_backend();
  REQUIRE(b->open(o));
  return b;
}

ParamRecord arm_update(std::uint64_t seq, std::uint8_t arm, InstrumentId inst) {
  ParamRecord r{};
  r.hdr.len = sizeof r;
  r.hdr.type = RecordType::Param;
  r.hdr.session_id = 1;
  r.hdr.seq = seq;
  r.hdr.engine_ts = Timestamp{kDay1Ns + static_cast<std::int64_t>(seq)};
  r.hdr.instrument = inst;
  r.origin = ParamUpdateMsg::Origin::Experiment;
  r.arm = arm;
  r.publish_seq = seq;
  std::strncpy(r.source, "ab:spread-1", sizeof r.source - 1);
  r.count = 1;
  r.field[0] = 0;
  r.value[0] = 80'000;
  return r;
}

}  // namespace

TEST_CASE("store.ab: the arms of a session, and the arm of each fill, order and update") {
  const std::string path = fresh("ab.db");
  const InstrumentTable table = fastmm::test::store_table();
  {
    GenericSection section;
    auto b = open_backend(path, section);
    SessionOpen s = fastmm::test::store_session(1, kDay1Ns);
    s.param_table = {{"half_spread_bps", 4}};
    s.params = {{"half_spread_bps", "5", 50'000, true}};
    s.params_complete = true;
    s.ab_test = "spread-1";
    s.ab_arms = {{"control", ""}, {"wide", "half_spread_bps=8"}};
    REQUIRE(b->session_open(s));
    REQUIRE(b->instruments(1, std::span<const Instrument>(table.data(), table.size())));
    b->begin();
    b->param(arm_update(1, 2, InstrumentId{0}));
    FillRecord f = fastmm::test::store_fill(
        1, 2, kDay1Ns + 10, Side::Buy, 10'000'000'000, 100'000, 0, 0, "E1");
    f.arm = 2;
    b->fill(f);
    FillRecord outside = fastmm::test::store_fill(
        1, 3, kDay1Ns + 11, Side::Sell, 10'000'000'000, 100'000, 0, 0, "E2");
    b->fill(outside);
    OrderRecord o = fastmm::test::store_order(1, 4, kDay1Ns + 12, 9, OrderState::Live);
    o.order.arm = 1;
    b->order(o);
    // The same arm again is still recorded: it is the assignment's history.
    b->param(arm_update(5, 2, InstrumentId{0}));
    b->param(arm_update(6, ParamUpdateMsg::kArmNone, InstrumentId{}));
    b->commit();
    CHECK(b->errors() == 0);
    CHECK(b->last_error().empty());
  }
  CHECK(text(path, "SELECT group_concat(name || '=' || params, ';') FROM ab_arms") ==
        "control=;wide=half_spread_bps=8");
  CHECK(text(path, "SELECT test FROM ab_arms WHERE arm = 1") == "spread-1");
  CHECK(text(path, "SELECT arm FROM fills WHERE exec_id = 'E1'") == "wide");
  CHECK(text(path, "SELECT arm FROM fills WHERE exec_id = 'E2'") == "<null>");
  CHECK(text(path, "SELECT arm FROM orders") == "control");
  CHECK(text(path,
             "SELECT group_concat(origin || ':' || COALESCE(arm, '-'), ' ') FROM "
             "param_updates ORDER BY seq") == "initial:- ab:wide ab:wide ab:");
  CHECK(text(path, "SELECT source FROM param_updates WHERE seq = 1") == "ab:spread-1");

  GenericSection section;
  section.values["path"] = path;
  BackendOptions o;
  o.config = &section;
  o.read_only = true;
  auto reader = make_sqlite_reader();
  REQUIRE(reader->open(o));
  QueryFilter all;
  const auto fills = reader->fills(all);
  REQUIRE(fills);
  const auto col = std::find(fills->columns.begin(), fills->columns.end(), "arm");
  REQUIRE(col != fills->columns.end());
  const auto k = static_cast<std::size_t>(col - fills->columns.begin());
  REQUIRE(fills->rows.size() == 2);
  CHECK(fills->rows[0][k] == "wide");
  CHECK(fills->rows[1][k].empty());
  const auto orders = reader->orders(all);
  REQUIRE(orders);
  CHECK(std::find(orders->columns.begin(), orders->columns.end(), "arm") != orders->columns.end());
}

TEST_CASE("store.ab: a store at schema 11 is copied, then migrated to 12, once") {
  const std::string path = fresh("ab_migrate.db");
  {
    sqlite3* db = nullptr;
    REQUIRE(
        sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) ==
        SQLITE_OK);
    REQUIRE(sqlite::migrate(db, 11));
    sqlite3_close_v2(db);
  }
  const std::string copy = path + ".v11.bak";
  {
    GenericSection section;
    auto b = open_backend(path, section);
    b->close();
  }
  REQUIRE(std::filesystem::exists(copy));
  CHECK(text(copy, "SELECT version FROM schema_version") == "11");
  CHECK(text(path, "SELECT version FROM schema_version") == "12");
  CHECK(text(path, "SELECT count(*) FROM pragma_table_info('fills') WHERE name = 'arm'") == "1");
  // A store already at the newest version is not copied again.
  std::filesystem::remove(copy);
  {
    GenericSection section;
    auto b = open_backend(path, section);
    b->close();
  }
  CHECK_FALSE(std::filesystem::exists(copy));
  std::error_code ec;
  std::filesystem::remove(path, ec);
}
