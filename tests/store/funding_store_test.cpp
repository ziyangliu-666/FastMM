// Funding payments in the SQLite store (schema 4): one row per payment, the part of realized PnL
// that is funding in the positions, the day roll-up, its views and the session totals, a version 3
// store migrated in place, and a restart's resume point that counts a payment as a venue event.
#include "../../src/store/sqlite_schema.hpp"
#include "store_test_util.hpp"

#include "fastmm/store/sqlite_store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::store;
using fastmm::test::kDay1Ns;
using fastmm::test::tmp_dir;

namespace {

constexpr std::int64_t kMs = 1'000'000;
constexpr std::int64_t kHour = 3'600'000 * kMs;

std::string fresh(const char* name) {
  const auto p = tmp_dir() / name;
  std::error_code ec;
  std::filesystem::remove(p, ec);
  std::filesystem::remove(p.string() + "-wal", ec);
  std::filesystem::remove(p.string() + "-shm", ec);
  return p.string();
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

std::size_t column(const Rows& r, std::string_view name) {
  for (std::size_t i = 0; i < r.columns.size(); ++i) {
    if (r.columns[i] == name) return i;
  }
  REQUIRE_MESSAGE(false, "no column " << name);
  return 0;
}

std::int64_t scalar(sqlite3* db, const char* sql) {
  sqlite3_stmt* st = nullptr;
  REQUIRE_MESSAGE(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK, sqlite3_errmsg(db));
  const std::int64_t v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
  sqlite3_finalize(st);
  return v;
}

std::int64_t scalar(const std::string& path, const char* sql) {
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
  const std::int64_t v = scalar(db, sql);
  sqlite3_close_v2(db);
  return v;
}

bool has_table(sqlite3* db, const char* name) {
  return scalar(db,
                (std::string("SELECT count(*) FROM sqlite_master WHERE name = '") + name + "'")
                    .c_str()) > 0;
}

// A position record of instrument 0 after `funding_raw` of funding and `realized_raw` in all.
PositionRecord position_after(std::uint64_t seq,
                              std::int64_t ts_ns,
                              std::int64_t realized_raw,
                              std::int64_t funding_raw) {
  PositionRecord p = fastmm::test::store_position(9, seq, ts_ns, 200'000'000, realized_raw, 0, 1);
  p.funding = Notional::from_raw(funding_raw);
  p.total_funding = p.funding;
  return p;
}

}  // namespace

TEST_CASE("store.funding: payments are rows, and every PnL total says how much of it is funding") {
  const std::string path = fresh("funding.db");
  const InstrumentTable table = fastmm::test::store_table();
  {
    GenericSection section;
    section.values["path"] = path;
    BackendOptions o;
    o.config = &section;
    o.engine_name = "test";
    auto b = make_sqlite_backend();
    REQUIRE(b->open(o));
    REQUIRE(b->session_open(fastmm::test::store_session(9, kDay1Ns)));
    REQUIRE(b->instruments(9, std::span<const Instrument>(table.data(), table.size())));
    b->begin();
    // A fill that realizes 1, then two payments: -1.5 paid at 08:00, +0.25 received at 16:00.
    b->fill(fastmm::test::store_fill(
        9, 1, kDay1Ns + kHour, Side::Sell, 100, 200'000'000, 100'000'000, 0, "E1"));
    b->position(position_after(2, kDay1Ns + kHour, 100'000'000, 0));
    FundingRecord paid = fastmm::test::store_funding(9,
                                                     3,
                                                     kDay1Ns + 8 * kHour + 5 * kMs,
                                                     kDay1Ns + 8 * kHour,
                                                     -150'000'000,
                                                     -150'000'000,
                                                     "7001");
    b->funding(paid);
    b->position(position_after(4, kDay1Ns + 8 * kHour, -50'000'000, -150'000'000));
    FundingRecord received = fastmm::test::store_funding(
        9, 5, kDay1Ns + 16 * kHour, kDay1Ns + 16 * kHour, 25'000'000, -125'000'000, "7002");
    received.hdr.flags |= RecordHeader::kReplayed;
    b->funding(received);
    b->position(position_after(6, kDay1Ns + 16 * kHour, -25'000'000, -125'000'000));
    // The same payment again under a later record: one row.
    FundingRecord again = paid;
    again.hdr.seq = 7;
    b->funding(again);
    b->commit();
    SessionClose c;
    c.session_id = 9;
    c.stopped_ns = kDay1Ns + 20 * kHour;
    c.stats.realized_pnl_raw = -25'000'000;
    c.stats.funding_raw = -125'000'000;
    REQUIRE(b->session_close(c));
    CHECK(b->errors() == 0);
    b->close();
  }
  CHECK(scalar(path, "SELECT count(*) FROM funding") == 2);
  CHECK(scalar(path, "SELECT exch_ns FROM funding WHERE funding_id = '7001'") ==
        kDay1Ns + 8 * kHour);
  CHECK(scalar(path, "SELECT replayed FROM funding WHERE funding_id = '7002'") == 1);
  CHECK(scalar(path, "SELECT funding_raw FROM pnl_daily") == -125'000'000);
  CHECK(scalar(path, "SELECT realized_raw FROM pnl_daily") == -25'000'000);
  CHECK(scalar(path, "SELECT funding_raw FROM pnl_by_day") == -125'000'000);
  CHECK(scalar(path, "SELECT funding_raw FROM pnl_by_currency") == -125'000'000);
  CHECK(scalar(path, "SELECT total_funding_raw FROM positions ORDER BY seq DESC LIMIT 1") ==
        -125'000'000);

  GenericSection section;
  auto reader = open_reader(path, section);
  {
    auto r = reader->funding(QueryFilter{});
    REQUIRE(r);
    REQUIRE(r->rows.size() == 2);
    CHECK(r->rows[0][column(*r, "amount")] == "-1.5");
    CHECK(r->rows[0][column(*r, "asset")] == "USDT");
    CHECK(r->rows[0][column(*r, "funding_id")] == "7001");
    CHECK(r->rows[0][column(*r, "symbol")] == "BTCUSDT");
    CHECK(r->rows[1][column(*r, "amount")] == "0.25");
    CHECK(r->rows[1][column(*r, "replayed")] == "1");
  }
  {
    auto r = reader->pnl(QueryFilter{});
    REQUIRE(r);
    REQUIRE(r->rows.size() == 1);
    CHECK(r->rows[0][column(*r, "realized")] == "-0.25");
    CHECK(r->rows[0][column(*r, "funding")] == "-1.25");
    CHECK(r->rows[0][column(*r, "net")] == "-0.25");
  }
  {
    auto r = reader->sessions(QueryFilter{});
    REQUIRE(r);
    REQUIRE(r->rows.size() == 1);
    CHECK(r->rows[0][column(*r, "funding")] == "-1.25");
  }
  {
    auto r = reader->positions(QueryFilter{});
    REQUIRE(r);
    REQUIRE(r->rows.size() == 1);
    CHECK(r->rows[0][column(*r, "funding")] == "-1.25");
  }
  QueryFilter f;
  f.engine = "test";
  auto rec = reader->recovery(f);
  REQUIRE(rec);
  CHECK(rec->funding == "-1.25");
  REQUIRE(rec->positions.size() == 1);
  CHECK(rec->positions[0].find("realized=-0.25 (funding -1.25)") != std::string::npos);
}

TEST_CASE("store.funding: a version 3 store gains the funding table, its old rows no funding") {
  const std::string path = fresh("funding_v3.db");
  {
    sqlite3* db = nullptr;
    REQUIRE(
        sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) ==
        SQLITE_OK);
    REQUIRE(sqlite::migrate(db, 3));
    CHECK_FALSE(has_table(db, "funding"));
    REQUIRE(sqlite::exec(db,
                         "INSERT INTO sessions (session_id, engine, strategy, session_epoch,"
                         " started_ns, started_day, version, build, config_hash, dry_run,"
                         " pnl_carry_raw, realized_raw) VALUES (3,'test','basic_mm',1,1,"
                         "'2024-03-04','0.1.0','b','0',0,0,700)"));
    REQUIRE(sqlite::exec(db,
                         "INSERT INTO positions VALUES (3,1,1,'2024-03-04',0,'BTCUSDT',0,0,700,0,"
                         "0,0,1,700,0,0,0)"));
    REQUIRE(sqlite::exec(db,
                         "INSERT INTO pnl_daily (session_id, day, instrument_id, symbol,"
                         " settlement_ccy, realized_raw) VALUES (3,'2024-03-04',0,'BTCUSDT',"
                         "'USDT',700)"));
    sqlite3_close_v2(db);
  }
  // Read as it is: a reader never migrates, and a version 3 store has no funding to show.
  {
    GenericSection section;
    auto reader = open_reader(path, section);
    auto p = reader->pnl(QueryFilter{});
    REQUIRE(p);
    REQUIRE(p->rows.size() == 1);
    CHECK(p->rows[0][column(*p, "funding")] == "0");
    CHECK(p->rows[0][column(*p, "realized")] == "0.000007");
    auto s = reader->sessions(QueryFilter{});
    REQUIRE(s);
    CHECK(s->rows[0][column(*s, "funding")] == "0");
    auto f = reader->funding(QueryFilter{});
    REQUIRE(f);
    CHECK(f->rows.empty());
    QueryFilter qf;
    qf.engine = "test";
    auto rec = reader->recovery(qf);
    REQUIRE(rec);
    CHECK(rec->funding == "0");
  }
  // A session's writer migrates it.
  {
    GenericSection section;
    section.values["path"] = path;
    BackendOptions o;
    o.config = &section;
    auto b = make_sqlite_backend();
    REQUIRE(b->open(o));
    b->close();
  }
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
  CHECK(scalar(db, "SELECT version FROM schema_version") == 4);
  CHECK(has_table(db, "funding"));
  CHECK(scalar(db, "SELECT funding_raw FROM pnl_daily") == 0);
  CHECK(scalar(db, "SELECT funding_raw FROM positions") == 0);
  CHECK(scalar(db, "SELECT realized_raw FROM pnl_by_day") == 700);
  CHECK(scalar(db, "SELECT funding_raw FROM pnl_by_day") == 0);
  CHECK(scalar(db, "SELECT count(*) FROM sessions WHERE funding_raw IS NULL") == 1);
  sqlite3_close_v2(db);
}

TEST_CASE("store.funding: a stored payment is a venue event a restart resumes from and knows") {
  const std::string path = fresh("funding_resume.db");
  const std::int64_t t0 = kDay1Ns / kMs + 8 * 3'600'000;  // venue ms of the payment
  {
    GenericSection section;
    section.values["path"] = path;
    BackendOptions o;
    o.config = &section;
    o.engine_name = "test";
    auto b = make_sqlite_backend();
    REQUIRE(b->open(o));
    SessionOpen s = fastmm::test::store_session(5, kDay1Ns);
    s.venues = {"binance_usdm"};
    REQUIRE(b->session_open(s));
    const InstrumentTable table = fastmm::test::store_table();
    REQUIRE(b->instruments(5, std::span<const Instrument>(table.data(), table.size())));
    b->begin();
    FillRecord fill =
        fastmm::test::store_fill(5, 1, (t0 - 60'000) * kMs, Side::Buy, 100, 1, 0, 0, "1001");
    fill.hdr.venue = VenueId{0};
    fill.hdr.exch_ts = Timestamp{(t0 - 60'000) * kMs};
    b->fill(fill);
    b->funding(fastmm::test::store_funding(5, 2, t0 * kMs, t0 * kMs, -1, -1, "88001"));
    b->funding(fastmm::test::store_funding(5, 3, t0 * kMs, (t0 - 200) * kMs, -1, -2, "88002"));
    b->commit();
    b->close();
  }
  GenericSection section;
  auto reader = open_reader(path, section);
  QueryFilter f;
  f.engine = "test";
  auto rec = reader->recovery(f);
  REQUIRE(rec);
  REQUIRE(rec->venue_resume.size() == 1);
  const Recovery::VenueResume& v = rec->venue_resume[0];
  CHECK(v.venue == "binance_usdm");
  // The last venue event is the payment, a minute after the last fill: the replay starts a
  // second before it, and both payments inside that second are known, under their prefix.
  CHECK(v.last_fill_ms == t0);
  CHECK(v.since_ms == t0 - Recovery::kResumeOverlapMs);
  std::vector<std::string> known = v.known_exec_ids;
  std::sort(known.begin(), known.end());
  CHECK(known == std::vector<std::string>{"funding:88001", "funding:88002"});
}
