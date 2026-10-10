// Strategy parameters over time in the SQLite store (schema 8): a session's starting set, every
// update the engine applied with its origin and source, an instrument's own value against the
// shared one, the values in effect at a time, what a restart changed, and the configuration a
// session ran with.
#include "store_test_util.hpp"

#include "fastmm/store/sqlite_store.hpp"

#include <sqlite3.h>

#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace fastmm;
using namespace fastmm::store;
using fastmm::test::kDay1Ns;
using fastmm::test::tmp_dir;

namespace {

constexpr std::int64_t kMinute = 60'000'000'000LL;
constexpr std::uint16_t kSpread = 0;  // bps
constexpr std::uint16_t kQty = 1;     // decimal
constexpr std::uint16_t kLevels = 2;  // int

std::string fresh(const char* name) {
  const auto p = tmp_dir() / name;
  std::error_code ec;
  std::filesystem::remove(p, ec);
  std::filesystem::remove(p.string() + "-wal", ec);
  std::filesystem::remove(p.string() + "-shm", ec);
  return p.string();
}

SessionOpen session(std::uint64_t id, std::int64_t started_ns, const char* spread) {
  SessionOpen s = fastmm::test::store_session(id, started_ns);
  s.config_toml = "[strategy.params]\nhalf_spread_bps = " + std::string(spread) + "\n";
  s.param_table = {{"half_spread_bps", 4}, {"quote_qty", 3}, {"levels", 0}};
  // 5 bps is 50'000 raw (Ratio: one bp is 10'000), 0.001 is 100'000 raw (1e-8).
  const std::int64_t spread_raw = std::stoll(spread) * 10'000;
  s.params = {{"half_spread_bps", spread, spread_raw, true},
              {"quote_qty", "0.001", 100'000, true},
              {"levels", "1", 1, true}};
  s.params_complete = true;
  return s;
}

ParamRecord update(std::uint64_t session,
                   std::uint64_t seq,
                   std::int64_t ts_ns,
                   ParamUpdateMsg::Origin origin,
                   const char* source,
                   std::vector<std::pair<std::uint16_t, std::int64_t>> values,
                   InstrumentId inst = InstrumentId{}) {
  ParamRecord r{};
  r.hdr.len = sizeof r;
  r.hdr.type = RecordType::Param;
  r.hdr.session_id = session;
  r.hdr.seq = seq;
  r.hdr.engine_ts = Timestamp{ts_ns};
  r.hdr.instrument = inst;
  r.origin = origin;
  r.publish_seq = seq;
  std::strncpy(r.source, source, sizeof r.source - 1);
  r.count = static_cast<std::uint32_t>(values.size());
  for (std::size_t k = 0; k < values.size(); ++k) {
    r.field[k] = values[k].first;
    r.value[k] = values[k].second;
  }
  return r;
}

std::unique_ptr<Reader> open_reader(const std::string& path, GenericSection& section) {
  section.values["path"] = path;
  BackendOptions o;
  o.config = &section;
  o.read_only = true;
  auto r = make_sqlite_reader();
  REQUIRE(r->open(o));
  return r;
}

// name/instrument -> value, of a params() answer.
std::map<std::string, std::string> values_of(const Rows& r) {
  std::map<std::string, std::string> out;
  for (const auto& row : r.rows) out[row[0] + "/" + row[1]] = row[2];
  return out;
}

std::int64_t scalar(const std::string& path, const char* sql) {
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
  sqlite3_stmt* st = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
  const std::int64_t v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
  sqlite3_finalize(st);
  sqlite3_close_v2(db);
  return v;
}

}  // namespace

TEST_CASE("store.params: the starting set, each update with its source, and the values at a time") {
  const std::string path = fresh("params.db");
  const InstrumentTable table = fastmm::test::store_table();
  {
    GenericSection section;
    section.values["path"] = path;
    BackendOptions o;
    o.config = &section;
    o.engine_name = "test";
    auto b = make_sqlite_backend();
    REQUIRE(b->open(o));
    // Session 1 starts at 5 bps; a drain widens it at +10 min; ETHUSDT gets its own quote_qty at
    // +20; the strategy's own publisher refreshes levels unchanged (counted, no rows) and then
    // changes it at +30; at +40 an update to every instrument replaces ETHUSDT's own quote_qty.
    REQUIRE(b->session_open(session(1, kDay1Ns, "5")));
    REQUIRE(b->instruments(1, std::span<const Instrument>(table.data(), table.size())));
    b->begin();
    using O = ParamUpdateMsg::Origin;
    b->param(
        update(1, 1, kDay1Ns + 10 * kMinute, O::Control, "scheduled:drain", {{kSpread, 90'000}}));
    b->param(update(
        1, 2, kDay1Ns + 20 * kMinute, O::Control, "manual", {{kQty, 200'000}}, InstrumentId{1}));
    b->param(update(1, 3, kDay1Ns + 25 * kMinute, O::Strategy, "", {{kLevels, 1}}));
    b->param(update(1, 4, kDay1Ns + 26 * kMinute, O::Strategy, "", {{kLevels, 1}}));
    b->param(update(1, 5, kDay1Ns + 30 * kMinute, O::Strategy, "", {{kLevels, 3}}));
    b->param(update(1, 6, kDay1Ns + 40 * kMinute, O::Control, "manual", {{kQty, 300'000}}));
    b->commit();
    SessionClose c;
    c.session_id = 1;
    c.stopped_ns = kDay1Ns + 50 * kMinute;
    REQUIRE(b->session_close(c));
    // Session 2, a restart: the configuration still says 5 bps, so the drain's 9 is gone.
    REQUIRE(b->session_open(session(2, kDay1Ns + 60 * kMinute, "5")));
    REQUIRE(b->instruments(2, std::span<const Instrument>(table.data(), table.size())));
    b->close();
  }
  CHECK(scalar(path, "SELECT version FROM schema_version") == kSqliteSchemaVersion);
  // Two unchanged refreshes of the strategy's own publisher: counted, not stored.
  CHECK(scalar(path, "SELECT param_refreshes FROM sessions WHERE session_id = 1") == 2);
  CHECK(scalar(path, "SELECT count(*) FROM param_updates WHERE session_id = 1") == 5);

  GenericSection section;
  auto reader = open_reader(path, section);
  ParamQuery q;
  q.session_id = 1;
  // At the start: the configured set, every parameter.
  q.at_ns = kDay1Ns + kMinute;
  auto at_start = reader->params(q);
  REQUIRE(at_start);
  CHECK(values_of(*at_start) == std::map<std::string, std::string>{{"half_spread_bps/*", "5"},
                                                                   {"levels/*", "1"},
                                                                   {"quote_qty/*", "0.001"}});
  // During the drain, with ETHUSDT's own quantity.
  q.at_ns = kDay1Ns + 22 * kMinute;
  auto mid = reader->params(q);
  REQUIRE(mid);
  CHECK(values_of(*mid) == std::map<std::string, std::string>{{"half_spread_bps/*", "9"},
                                                              {"levels/*", "1"},
                                                              {"quote_qty/*", "0.001"},
                                                              {"quote_qty/ETHUSDT", "0.002"}});
  for (const auto& row : mid->rows) {
    if (row[0] == "half_spread_bps") {
      CHECK(row[4] == "control");
      CHECK(row[5] == "scheduled:drain");
    }
  }
  // One instrument: its own value hides the shared one.
  q.instrument = "ETHUSDT";
  auto eth = reader->params(q);
  REQUIRE(eth);
  CHECK(values_of(*eth).count("quote_qty/*") == 0);
  CHECK(values_of(*eth).at("quote_qty/ETHUSDT") == "0.002");
  q.instrument.clear();
  // At the end: the update to every instrument replaced ETHUSDT's own value.
  q.at_ns = 0;
  auto end = reader->params(q);
  REQUIRE(end);
  CHECK(values_of(*end) == std::map<std::string, std::string>{{"half_spread_bps/*", "9"},
                                                              {"levels/*", "3"},
                                                              {"quote_qty/*", "0.003"}});

  // Without a session: the newest of the engine that had started by then.
  ParamQuery by_time;
  by_time.engine = "test";
  by_time.at_ns = kDay1Ns + 45 * kMinute;
  auto newest = reader->params(by_time);
  REQUIRE(newest);
  REQUIRE_FALSE(newest->rows.empty());
  CHECK(newest->rows[0][6] == "1");
  by_time.at_ns = kDay1Ns - kMinute;
  CHECK_FALSE(reader->params(by_time));  // nothing had started

  // The history: the starting set, then each stored update in order.
  QueryFilter f;
  f.session_id = 1;
  auto changes = reader->param_changes(f);
  REQUIRE(changes);
  CHECK(changes->rows.size() == 3 + 1 + 1 + 1 + 1);
  CHECK(changes->rows[3][3] == "control");
  CHECK(changes->rows[3][4] == "scheduled:drain");
  CHECK(changes->rows[3][7] == "9");
  f.instrument = "BTCUSDT";  // the shared rows, not ETHUSDT's own
  auto btc = reader->param_changes(f);
  REQUIRE(btc);
  CHECK(btc->rows.size() == changes->rows.size() - 1);

  // What the restart changed: session 1 ended with what session 2 no longer starts with.
  auto diff = reader->param_diff(0, 2);
  REQUIRE(diff);
  std::map<std::string, std::pair<std::string, std::string>> d;
  for (const auto& row : diff->rows) d[row[0] + "/" + row[1]] = {row[2], row[3]};
  CHECK(d == std::map<std::string, std::pair<std::string, std::string>>{
                 {"half_spread_bps/*", {"9", "5"}},
                 {"levels/*", {"3", "1"}},
                 {"quote_qty/*", {"0.003", "0.001"}}});
  CHECK_FALSE(reader->param_diff(0, 1));  // no earlier session

  ParamQuery cfg;
  cfg.session_id = 2;
  auto toml = reader->session_config(cfg);
  REQUIRE(toml);
  CHECK(*toml == "[strategy.params]\nhalf_spread_bps = 5\n");
}

TEST_CASE("store.params: a strategy without values keeps the configured ones, marked incomplete") {
  const std::string path = fresh("params-partial.db");
  {
    GenericSection section;
    section.values["path"] = path;
    BackendOptions o;
    o.config = &section;
    auto b = make_sqlite_backend();
    REQUIRE(b->open(o));
    SessionOpen s = fastmm::test::store_session(3, kDay1Ns);
    s.params = {{"half_spread_bps", "7", 0, false}};
    REQUIRE(b->session_open(s));
    b->close();
  }
  CHECK(scalar(path, "SELECT complete FROM param_updates WHERE session_id = 3 AND seq = 0") == 0);
  CHECK(scalar(path, "SELECT raw IS NULL FROM param_values WHERE session_id = 3") == 1);
  GenericSection section;
  auto reader = open_reader(path, section);
  ParamQuery q;
  q.session_id = 3;
  auto rows = reader->params(q);
  REQUIRE(rows);
  REQUIRE(rows->rows.size() == 1);
  CHECK(rows->rows[0][2] == "7");
  CHECK(rows->rows[0][4] == "initial");
}
