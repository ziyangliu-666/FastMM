// Where a restarted session's execution replay resumes: the venue's time of the last stored fill
// per venue, the trade ids stored in the overlap before it, the last numeric trade id per symbol,
// and the engine-clock fallback of a store written before fills kept their venue time.
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
// The engine clock of the fills below runs 30 s behind the venue's: nothing the resume point
// is computed from may come from it.
constexpr std::int64_t kEngineBehindNs = 30'000 * kMs;

std::string fresh(const char* name) {
  const auto p = tmp_dir() / name;
  std::error_code ec;
  std::filesystem::remove(p, ec);
  std::filesystem::remove(p.string() + "-wal", ec);
  std::filesystem::remove(p.string() + "-shm", ec);
  return p.string();
}

// BTCUSDT on venue 0 ("binance"), ETHUSDT on venue 1 ("bybit").
InstrumentTable two_venues() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.base = "BTC";
  i.quote = "USDT";
  i.asset_class = AssetClass::Spot;
  i.tick = Price::from_decimal("0.01").value();
  i.lot = Qty::from_decimal("0.001").value();
  i.flags = Instrument::kEnabled;
  i.venue = VenueId{0};
  REQUIRE(t.add(i));
  i.symbol = "ETHUSDT";
  i.base = "ETH";
  i.venue = VenueId{1};
  REQUIRE(t.add(i));
  return t;
}

struct Writer {
  GenericSection section;
  std::unique_ptr<Backend> backend = make_sqlite_backend();
  std::uint64_t seq = 0;
  std::uint64_t session = 0;

  Writer(const std::string& path, std::uint64_t id) : session(id) {
    section.values["path"] = path;
    BackendOptions o;
    o.config = &section;
    o.engine_name = "test";
    REQUIRE(backend->open(o));
    SessionOpen s = fastmm::test::store_session(id, kDay1Ns);
    s.venues = {"binance", "bybit"};
    REQUIRE(backend->session_open(s));
    const InstrumentTable t = two_venues();
    REQUIRE(backend->instruments(id, std::span<const Instrument>(t.data(), t.size())));
    backend->begin();
  }
  // A fill on instrument `inst` (its venue has the same id) traded at venue time `exch_ms`.
  void fill(std::uint32_t inst, std::int64_t exch_ms, const std::string& exec_id) {
    FillRecord r = fastmm::test::store_fill(
        session, ++seq, exch_ms * kMs - kEngineBehindNs, Side::Buy, 100, 1, 0, 0, exec_id);
    r.hdr.instrument = InstrumentId{inst};
    r.hdr.venue = VenueId{static_cast<std::uint8_t>(inst)};
    r.hdr.exch_ts = Timestamp{exch_ms * kMs};
    backend->fill(r);
  }
  ~Writer() {
    backend->commit();
    backend->close();
  }
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
};

Recovery recover(const std::string& path) {
  GenericSection section;
  section.values["path"] = path;
  BackendOptions o;
  o.config = &section;
  o.read_only = true;
  auto reader = make_sqlite_reader();
  REQUIRE(reader->open(o));
  QueryFilter f;
  f.engine = "test";
  auto r = reader->recovery(f);
  REQUIRE(r);
  REQUIRE(r->found);
  return *r;
}

const Recovery::VenueResume* venue(const Recovery& r, std::string_view name) {
  for (const Recovery::VenueResume& v : r.venue_resume) {
    if (v.venue == name) return &v;
  }
  return nullptr;
}

std::vector<std::string> sorted(std::vector<std::string> v) {
  std::sort(v.begin(), v.end());
  return v;
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

constexpr std::int64_t kT0 = kDay1Ns / kMs + 3'600'000;  // venue ms of binance's last fill
constexpr std::int64_t kT1 = kT0 - 7'000;                // and of bybit's

}  // namespace

TEST_CASE("store.resume: a fill's venue time round trips and each venue resumes in its own clock") {
  const std::string path = fresh("resume_venue_time.db");
  {
    Writer w(path, 5);
    w.fill(0, kT0 - 5'000, "101");
    w.fill(0, kT0 - 1'000, "102");  // exactly at the start: inclusive
    w.fill(0, kT0 - 400, "103");
    w.fill(0, kT0, "104");
    w.fill(1, kT1 - 2'000, "a-1");
    w.fill(1, kT1, "a-2");
  }
  CHECK(scalar(path, "SELECT exch_ns FROM fills WHERE exec_id = '104'") == kT0 * kMs);
  CHECK(scalar(path, "SELECT ts_ns FROM fills WHERE exec_id = '104'") ==
        kT0 * kMs - kEngineBehindNs);

  const Recovery r = recover(path);
  REQUIRE(r.venue_resume.size() == 2);
  const Recovery::VenueResume* b = venue(r, "binance");
  REQUIRE(b != nullptr);
  CHECK(b->venue_id == 0);
  CHECK(b->last_fill_ms == kT0);
  CHECK(b->since_ms == kT0 - Recovery::kResumeOverlapMs);
  CHECK_FALSE(b->shrunk);
  CHECK(sorted(b->known_exec_ids) == std::vector<std::string>{"102", "103", "104"});
  const Recovery::VenueResume* y = venue(r, "bybit");
  REQUIRE(y != nullptr);
  CHECK(y->venue_id == 1);
  CHECK(y->last_fill_ms == kT1);
  CHECK(y->since_ms == kT1 - Recovery::kResumeOverlapMs);
  CHECK(y->known_exec_ids == std::vector<std::string>{"a-2"});

  // The last numeric trade id per symbol; bybit's ids are not numbers.
  REQUIRE(r.last_trade_ids.size() == 1);
  CHECK(r.last_trade_ids[0].venue == "binance");
  CHECK(r.last_trade_ids[0].symbol == "BTCUSDT");
  CHECK(r.last_trade_ids[0].last_id == 104);

  // The engine-clock fallback is still worked out, for a venue the store has no entry for.
  CHECK(r.last_fill_ns == kT0 * kMs - kEngineBehindNs);
  CHECK(r.fallback_since_ms == (kT0 * kMs - kEngineBehindNs - Recovery::kFallbackOverlapNs) / kMs);
}

TEST_CASE("store.resume: more ids in the overlap than an attach carries move the start later") {
  SUBCASE("one fill per millisecond") {
    const std::string path = fresh("resume_shrink.db");
    {
      Writer w(path, 6);
      for (int i = 199; i >= 0; --i) w.fill(0, kT0 - i, "E" + std::to_string(i));
    }
    const Recovery r = recover(path);
    const Recovery::VenueResume* b = venue(r, "binance");
    REQUIRE(b != nullptr);
    CHECK(b->shrunk);
    REQUIRE(b->known_exec_ids.size() == Recovery::kMaxKnownExecIds);
    // Every stored fill at or after the start is listed: none is left to be booked again.
    CHECK(b->since_ms == kT0 - static_cast<std::int64_t>(Recovery::kMaxKnownExecIds) + 1);
    CHECK(std::count(b->known_exec_ids.begin(), b->known_exec_ids.end(), "E0") == 1);
    CHECK(std::count(b->known_exec_ids.begin(), b->known_exec_ids.end(), "E127") == 1);
    CHECK(std::count(b->known_exec_ids.begin(), b->known_exec_ids.end(), "E128") == 0);
  }
  SUBCASE("a millisecond that does not fit whole is left out whole") {
    const std::string path = fresh("resume_shrink_ms.db");
    {
      Writer w(path, 7);
      for (int k = 0; k < 3; ++k) w.fill(0, kT0 - 500, "S" + std::to_string(k));
      for (int i = 126; i >= 0; --i) w.fill(0, kT0 - i, "E" + std::to_string(i));
    }
    const Recovery r = recover(path);
    const Recovery::VenueResume* b = venue(r, "binance");
    REQUIRE(b != nullptr);
    CHECK(b->shrunk);
    CHECK(b->since_ms == kT0 - 499);
    CHECK(b->known_exec_ids.size() == 127);
    CHECK(std::count(b->known_exec_ids.begin(), b->known_exec_ids.end(), "S0") == 0);
  }
}

TEST_CASE("store.resume: a version 2 store opens and resumes from the engine clock") {
  const std::string path = fresh("resume_v2.db");
  const std::int64_t last_ns = kT0 * kMs;
  {
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open_v2(
                path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) ==
            SQLITE_OK);
    REQUIRE(sqlite::migrate(db, 2));
    REQUIRE(sqlite::exec(db,
                         "INSERT INTO sessions (session_id, engine, strategy, session_epoch,"
                         " started_ns, started_day, version, build, config_hash, dry_run,"
                         " pnl_carry_raw) VALUES (3,'test','basic_mm',1,1,'2024-03-04','0.1.0',"
                         "'b','0',0,0)"));
    // Fills 25 s, 15 s, 5 s and 0 s before the last, in the engine's clock.
    std::string rows = "INSERT INTO fills VALUES";
    const std::int64_t back_s[] = {25, 15, 5, 0};
    for (std::size_t i = 0; i < 4; ++i) {
      rows += (i == 0 ? " " : ", ");
      rows += "(3," + std::to_string(i + 1) + "," +
              std::to_string(last_ns - back_s[i] * 1'000 * kMs) +
              ",'2024-03-04',0,0,'BTCUSDT','c','v','" + std::to_string(200 + i) +
              "','Buy','Maker',1,1,1,1,0,0,0,'quote',1,1,0,0,0,0)";
    }
    REQUIRE(sqlite::exec(db, rows.c_str()));
    sqlite3_close_v2(db);
  }
  const auto check = [&](const Recovery& r) {
    CHECK(r.venue_resume.empty());
    CHECK(r.last_fill_ns == last_ns);
    CHECK(r.fallback_since_ms == (last_ns - Recovery::kFallbackOverlapNs) / kMs);
    CHECK(sorted(r.fallback_exec_ids) == std::vector<std::string>{"201", "202", "203"});
    // Trade ids do not depend on a clock: the exact start works for an old store too.
    REQUIRE(r.last_trade_ids.size() == 1);
    CHECK(r.last_trade_ids[0].venue.empty());
    CHECK(r.last_trade_ids[0].venue_id == 0);
    CHECK(r.last_trade_ids[0].last_id == 203);
  };
  // Read as it is (a reader never migrates) ...
  check(recover(path));
  // ... and after a session's writer migrated it: the old rows have no venue time.
  {
    GenericSection section;
    section.values["path"] = path;
    BackendOptions o;
    o.config = &section;
    auto backend = make_sqlite_backend();
    REQUIRE(backend->open(o));
    backend->close();
  }
  CHECK(scalar(path, "SELECT version FROM schema_version") == kSqliteSchemaVersion);
  CHECK(scalar(path, "SELECT exch_ns FROM fills WHERE exec_id = '203'") == 0);
  check(recover(path));
}
