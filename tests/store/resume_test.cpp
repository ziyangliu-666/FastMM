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

// A session that died inside its start-up replay (Writer's last argument).
constexpr bool kDiedInReplay = false;

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

  Writer(const std::string& path,
         std::uint64_t id,
         std::int64_t started_ns = kDay1Ns,
         const InstrumentTable& t = two_venues(),
         std::vector<std::string> venues = {"binance", "bybit"},
         bool reconciled = true)
      : session(id) {
    section.values["path"] = path;
    BackendOptions o;
    o.config = &section;
    o.engine_name = "test";
    REQUIRE(backend->open(o));
    SessionOpen s = fastmm::test::store_session(id, started_ns);
    s.venues = std::move(venues);
    REQUIRE(backend->session_open(s));
    REQUIRE(backend->instruments(id, std::span<const Instrument>(t.data(), t.size())));
    backend->begin();
    // A session gets past its start-up replay on every venue, unless the test says it died there.
    if (reconciled) {
      for (std::uint8_t v = 0; v < 2; ++v)
        backend->replayed(fastmm::test::store_replayed(session, ++seq, v));
    }
  }
  // A position snapshot of instrument `inst`.
  void position(std::uint32_t inst, std::int64_t qty_raw) {
    PositionRecord r = fastmm::test::store_position(session, ++seq, kDay1Ns, qty_raw, 0, 0, 1);
    r.hdr.instrument = InstrumentId{inst};
    r.hdr.venue = VenueId{static_cast<std::uint8_t>(inst)};
    backend->position(r);
  }
  // A fill of instrument `inst` that leaves `position_raw`, with no position row after it.
  void fill_leaving(std::uint32_t inst,
                    std::int64_t exch_ms,
                    const std::string& exec_id,
                    std::int64_t position_raw) {
    FillRecord r = fastmm::test::store_fill(
        session, ++seq, exch_ms * kMs - kEngineBehindNs, Side::Buy, 100, 1, 0, 0, exec_id);
    r.hdr.instrument = InstrumentId{inst};
    r.hdr.venue = VenueId{static_cast<std::uint8_t>(inst)};
    r.hdr.exch_ts = Timestamp{exch_ms * kMs};
    r.position_qty = Qty::from_raw(position_raw);
    backend->fill(r);
  }
  // An order of instrument `inst`: the session got past its start-up replay on that venue.
  void order(std::uint32_t inst) {
    ++seq;
    OrderRecord r = fastmm::test::store_order(
        session, seq, kDay1Ns, static_cast<std::uint32_t>(seq), OrderState::Live);
    r.hdr.instrument = InstrumentId{inst};
    r.hdr.venue = VenueId{static_cast<std::uint8_t>(inst)};
    r.order.instrument = InstrumentId{inst};
    r.order.venue = VenueId{static_cast<std::uint8_t>(inst)};
    backend->order(r);
  }
  // The session records its shutdown (without it, it crashed).
  void close_cleanly() {
    backend->commit();
    SessionClose c;
    c.session_id = session;
    c.stopped_ns = kDay1Ns + 1;
    REQUIRE(backend->session_close(c));
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
    REQUIRE(
        sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) ==
        SQLITE_OK);
    REQUIRE(sqlite::migrate(db, 2));
    REQUIRE(sqlite::exec(db,
                         "INSERT INTO sessions (session_id, engine, strategy, session_epoch,"
                         " started_ns, started_day, version, build, config_hash, dry_run,"
                         " pnl_carry_raw, clean_shutdown) VALUES (3,'test','basic_mm',1,1,"
                         "'2024-03-04','0.1.0','b','0',0,0,1)"));
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

namespace {

// BTCUSDT on venue 0 ("quote") and on venue 1 ("hedge"): one symbol, two venues.
InstrumentTable same_symbol() {
  InstrumentTable t = two_venues();
  InstrumentTable out;
  for (Instrument i : t) {
    i.symbol = "BTCUSDT";
    i.base = "BTC";
    REQUIRE(out.add(i));
  }
  return out;
}

const Recovery::PositionState* position_of(const Recovery& r,
                                           std::string_view venue,
                                           std::string_view symbol) {
  for (const Recovery::PositionState& p : r.position_state) {
    if (p.venue == venue && p.symbol == symbol) return &p;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("store.resume: two venues trading one symbol keep their own positions") {
  const std::string path = fresh("resume_same_symbol.db");
  {
    Writer w(path, 5, kDay1Ns, same_symbol(), {"quote", "hedge"});
    w.position(0, 100'000);   // +0.001 on the quote venue
    w.position(1, -100'000);  // -0.001 on the hedge venue
  }
  const Recovery r = recover(path);
  REQUIRE(r.position_state.size() == 2);
  const Recovery::PositionState* q = position_of(r, "quote", "BTCUSDT");
  const Recovery::PositionState* h = position_of(r, "hedge", "BTCUSDT");
  REQUIRE(q != nullptr);
  REQUIRE(h != nullptr);
  CHECK(q->venue_id == 0);
  CHECK(q->qty_raw == 100'000);
  CHECK(h->venue_id == 1);
  CHECK(h->qty_raw == -100'000);
}

// The store commits whatever the ring holds when it drains: a crash can keep a fill whose
// position row had not reached it. The fill carries the position it left.
TEST_CASE("store.resume: a fill stored without the position row after it sets the position") {
  const std::string path = fresh("resume_fill_last.db");
  {
    Writer w(path, 5);
    w.position(0, 100'000);
    w.fill_leaving(0, kT0, "104", 200'000);
  }
  const Recovery r = recover(path);
  const Recovery::PositionState* b = position_of(r, "binance", "BTCUSDT");
  REQUIRE(b != nullptr);
  CHECK(b->qty_raw == 200'000);
}

// A session that crashed before it stored a fill (here: after restoring one position) holds no
// resume point and only part of the positions; the restart takes the rest from the session before.
TEST_CASE("store.resume: what the newest session did not record comes from the one before") {
  const std::string path = fresh("resume_chain.db");
  const std::int64_t start5 = kDay1Ns;
  const std::int64_t start6 = kDay1Ns + 60'000 * kMs;
  {
    Writer w(path, 5, start5);
    w.fill_leaving(0, kT0 - 400, "103", 100'000);
    w.fill_leaving(0, kT0, "104", 200'000);
    w.fill_leaving(1, kT1, "a-2", -300'000);
  }
  {
    Writer w(path, 6, start6);
    w.position(0, 200'000);  // restored, then it died
  }
  const Recovery r = recover(path);
  CHECK(r.session_id == 6);
  // Positions: binance from session 6, bybit (nothing in 6) from session 5.
  REQUIRE(r.position_state.size() == 2);
  REQUIRE(position_of(r, "binance", "BTCUSDT") != nullptr);
  CHECK(position_of(r, "binance", "BTCUSDT")->qty_raw == 200'000);
  REQUIRE(position_of(r, "bybit", "ETHUSDT") != nullptr);
  CHECK(position_of(r, "bybit", "ETHUSDT")->qty_raw == -300'000);
  // Resume points: session 5's, the newest that stored a fill of each venue.
  REQUIRE(r.venue_resume.size() == 2);
  const Recovery::VenueResume* b = venue(r, "binance");
  REQUIRE(b != nullptr);
  CHECK(b->last_fill_ms == kT0);
  CHECK(b->since_ms == kT0 - Recovery::kResumeOverlapMs);
  CHECK(sorted(b->known_exec_ids) == std::vector<std::string>{"103", "104"});
  REQUIRE(venue(r, "bybit") != nullptr);
  CHECK(venue(r, "bybit")->last_fill_ms == kT1);
  REQUIRE(r.last_trade_ids.size() == 1);
  CHECK(r.last_trade_ids[0].last_id == 104);
  CHECK(r.last_fill_ns == kT0 * kMs - kEngineBehindNs);
  // Neither shut down cleanly: a venue with no stored fill replays from the oldest start.
  CHECK(r.unbooked_since_ms == (start5 - Recovery::kFallbackOverlapNs) / kMs);
}

// The crash sequence of a live test: a session trades and is killed, the next one's replay books
// the one fill it missed and is killed seconds later, and so on. Each restart's window reaches the
// executions of the sessions before the newest, so every one of them has to be known.
TEST_CASE("store.resume: the ids of every earlier session inside the window are known") {
  const std::string path = fresh("resume_all_sessions.db");
  const std::int64_t minute = 60'000 * kMs;
  {
    Writer w(path, 5, kDay1Ns);
    w.fill(1, kT1 - 30'000, "2966124001");  // long before: outside every window
    w.fill(1, kT1, "2966124872");
    w.fill(1, kT1, "2966124873");
  }
  {
    Writer w(path, 6, kDay1Ns + minute, two_venues(), {"binance", "bybit"}, kDiedInReplay);
    w.fill(1, kT1 + 60, "2966124877");  // what session 5 missed, replayed, then killed
  }
  {
    const Recovery r = recover(path);
    const Recovery::VenueResume* y = venue(r, "bybit");
    REQUIRE(y != nullptr);
    // Session 6 did not get past its replay, so the start is session 5's.
    CHECK(y->last_fill_ms == kT1);
    CHECK(y->since_ms == kT1 - Recovery::kResumeOverlapMs);
    CHECK(sorted(y->known_exec_ids) ==
          std::vector<std::string>{"2966124872", "2966124873", "2966124877"});
    CHECK(r.duplicate_count == 0);
  }
  // A third session that found nothing new and was killed, then a fourth: the same answer.
  {
    Writer w(path, 7, kDay1Ns + 2 * minute, two_venues(), {"binance", "bybit"}, kDiedInReplay);
    w.position(1, 100'000);
  }
  {
    const Recovery r = recover(path);
    CHECK(r.session_id == 7);
    const Recovery::VenueResume* y = venue(r, "bybit");
    REQUIRE(y != nullptr);
    CHECK(sorted(y->known_exec_ids) ==
          std::vector<std::string>{"2966124872", "2966124873", "2966124877"});
  }
  // A session that got past its replay and stored a later fill: the start moves to that fill, and
  // the ids inside its overlap are still those of every session.
  {
    Writer w(path, 8, kDay1Ns + 3 * minute);
    w.fill(1, kT1 + 500, "2966124900");
  }
  const Recovery r = recover(path);
  const Recovery::VenueResume* y = venue(r, "bybit");
  REQUIRE(y != nullptr);
  CHECK(y->last_fill_ms == kT1 + 500);
  CHECK(y->since_ms == kT1 + 500 - Recovery::kResumeOverlapMs);
  CHECK(sorted(y->known_exec_ids) ==
        std::vector<std::string>{"2966124872", "2966124873", "2966124877", "2966124900"});
}

// A session killed inside its start-up replay can hold a fill newer than one the replay had not
// reached (an order left resting filled meanwhile). Its last fill is not where the record ends.
TEST_CASE("store.resume: a session that died inside its replay does not move the start") {
  const std::string path = fresh("resume_mid_replay.db");
  const std::int64_t minute = 60'000 * kMs;
  {
    Writer w(path, 5, kDay1Ns);
    w.order(0);
    w.fill(0, kT0 - 200, "E1");
    w.fill(0, kT0, "E2");
  }
  {
    Writer w(path, 6, kDay1Ns + minute, two_venues(), {"binance", "bybit"}, kDiedInReplay);
    w.fill(0, kT0 + 9'000, "E9");  // live, before the replay had read kT0 .. kT0 + 9 s
  }
  {
    Writer w(path, 7, kDay1Ns + 2 * minute, two_venues(), {"binance", "bybit"}, kDiedInReplay);
    w.fill(0, kT0 + 4'000, "E5");  // replayed; killed before the rest
  }
  const Recovery r = recover(path);
  const Recovery::VenueResume* b = venue(r, "binance");
  REQUIRE(b != nullptr);
  CHECK(b->last_fill_ms == kT0);
  CHECK(b->since_ms == kT0 - Recovery::kResumeOverlapMs);
  CHECK_FALSE(b->shrunk);
  CHECK(sorted(b->known_exec_ids) == std::vector<std::string>{"E1", "E2", "E5", "E9"});

  SUBCASE("sessions recorded before schema 6 are judged by their orders and their shutdown") {
    {
      sqlite3* db = nullptr;
      REQUIRE(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
      REQUIRE(sqlite::exec(db, "UPDATE session_venues SET replayed_seq = NULL"));
      sqlite3_close_v2(db);
    }
    // Session 5 placed an order on binance; 6 and 7 did not and recorded no shutdown.
    const Recovery old = recover(path);
    const Recovery::VenueResume* b0 = venue(old, "binance");
    REQUIRE(b0 != nullptr);
    CHECK(b0->last_fill_ms == kT0);
    CHECK(sorted(b0->known_exec_ids) == std::vector<std::string>{"E1", "E2", "E5", "E9"});
  }
  SUBCASE("the next session that gets past its replay moves the start again") {
    {
      Writer w(path, 8, kDay1Ns + 3 * minute);
      w.fill(0, kT0 + 20'000, "E20");
    }
    {
      Writer w(path, 9, kDay1Ns + 4 * minute, two_venues(), {"binance", "bybit"}, kDiedInReplay);
      w.fill(0, kT0 + 20'400, "E21");
    }
    const Recovery r2 = recover(path);
    const Recovery::VenueResume* b2 = venue(r2, "binance");
    REQUIRE(b2 != nullptr);
    CHECK(b2->last_fill_ms == kT0 + 20'000);
    CHECK(sorted(b2->known_exec_ids) == std::vector<std::string>{"E20", "E21"});
  }
}

// A venue is its name: its id is its place in one session's configuration.
TEST_CASE("store.resume: the ids of a venue that had another id in an earlier session are known") {
  const std::string path = fresh("resume_venue_moved.db");
  {
    Writer w(path, 5);  // binance = 0, bybit = 1
    w.fill(1, kT1, "a-1");
    w.fill(0, kT1, "771");
  }
  {
    // bybit = 0 now, and a venue the first session did not have.
    InstrumentTable t;
    Instrument i{};
    i.symbol = "ETHUSDT";
    i.base = "ETH";
    i.quote = "USDT";
    i.asset_class = AssetClass::Spot;
    i.tick = Price::from_decimal("0.01").value();
    i.lot = Qty::from_decimal("0.001").value();
    i.flags = Instrument::kEnabled;
    i.venue = VenueId{0};
    REQUIRE(t.add(i));
    i.symbol = "BTC-USDT";
    i.base = "BTC";
    i.venue = VenueId{1};
    REQUIRE(t.add(i));
    Writer w(path, 6, kDay1Ns + 60'000 * kMs, t, {"bybit", "okx"}, kDiedInReplay);
    w.fill(0, kT1 + 100, "a-2");
  }
  const Recovery r = recover(path);
  const Recovery::VenueResume* y = venue(r, "bybit");
  REQUIRE(y != nullptr);
  CHECK(y->venue_id == 0);  // in the newest session
  CHECK(sorted(y->known_exec_ids) == std::vector<std::string>{"a-1", "a-2"});
  // binance's fill of the same millisecond is not bybit's.
  const Recovery::VenueResume* b = venue(r, "binance");
  REQUIRE(b != nullptr);
  CHECK(b->known_exec_ids == std::vector<std::string>{"771"});
}

// Binance resumes at a trade id. A session that died inside its replay can hold an id above ones
// that replay had not read (a resting order filled meanwhile): the start stays after the last
// session that got past its replay, and what the dead one stored above it is listed to be skipped.
TEST_CASE("store.resume: a session that died inside its replay does not move the trade id") {
  const std::string path = fresh("resume_marks_mid_replay.db");
  const std::int64_t minute = 60'000 * kMs;
  {
    Writer w(path, 5);
    w.fill(0, kT0 - 100, "99");
    w.fill(0, kT0, "100");
  }
  {
    Writer w(path, 6, kDay1Ns + minute, two_venues(), {"binance", "bybit"}, kDiedInReplay);
    w.fill(0, kT0 + 5'000, "105");  // live: 101 .. 104 traded while nothing ran
    w.fill(0, kT0 + 1'000, "101");  // replayed, then it was killed
  }
  {
    Writer w(path, 7, kDay1Ns + 2 * minute, two_venues(), {"binance", "bybit"}, kDiedInReplay);
    w.fill(0, kT0 + 2'000, "102");
  }
  const Recovery r = recover(path);
  REQUIRE(r.last_trade_ids.size() == 1);
  CHECK(r.last_trade_ids[0].venue == "binance");
  CHECK(r.last_trade_ids[0].symbol == "BTCUSDT");
  CHECK(r.last_trade_ids[0].last_id == 100);
  CHECK(r.last_trade_ids[0].known_after == std::vector<std::int64_t>{101, 102, 105});
  // By time (behind a gateway) the same: from session 5's last fill, every id since known.
  const Recovery::VenueResume* b = venue(r, "binance");
  REQUIRE(b != nullptr);
  CHECK(b->last_fill_ms == kT0);
  CHECK(sorted(b->known_exec_ids) == std::vector<std::string>{"100", "101", "102", "105", "99"});

  // The session that finishes the replay: 103 and 104 are in, and the start moves past 105.
  {
    Writer w(path, 8, kDay1Ns + 3 * minute);
    w.fill(0, kT0 + 3'000, "103");
    w.fill(0, kT0 + 4'000, "104");
  }
  const Recovery done = recover(path);
  REQUIRE(done.last_trade_ids.size() == 1);
  CHECK(done.last_trade_ids[0].last_id == 105);
  CHECK(done.last_trade_ids[0].known_after.empty());
  REQUIRE(venue(done, "binance") != nullptr);
  CHECK(venue(done, "binance")->last_fill_ms == kT0 + 5'000);
}

// No session ever got past its replay: nothing says where the record is whole, so there is no
// trade id to go on from and the replay starts where the sessions began.
TEST_CASE("store.resume: a store whose sessions all died inside their replay starts at the first") {
  const std::string path = fresh("resume_no_anchor.db");
  const std::int64_t start5 = kDay1Ns + 3'000'000 * kMs;  // 50 min in: 10 min before kT0
  {
    Writer w(path, 5, start5, two_venues(), {"binance", "bybit"}, kDiedInReplay);
    w.fill(0, kT0, "100");
  }
  { Writer w(path, 6, start5 + 60'000 * kMs, two_venues(), {"binance", "bybit"}, kDiedInReplay); }
  const Recovery r = recover(path);
  CHECK(r.last_trade_ids.empty());
  const Recovery::VenueResume* b = venue(r, "binance");
  REQUIRE(b != nullptr);
  CHECK(b->last_fill_ms == kT0);
  CHECK(r.unbooked_since_ms == (start5 - Recovery::kFallbackOverlapNs) / kMs);
  CHECK(b->since_ms == r.unbooked_since_ms);
  CHECK(b->known_exec_ids == std::vector<std::string>{"100"});
}

// Binance resumes at the trade id after the highest one stored, whichever session stored it.
TEST_CASE("store.resume: the last trade id is the highest of every session") {
  const std::string path = fresh("resume_marks.db");
  {
    Writer w(path, 5);
    w.fill(0, kT0, "210");
  }
  {
    Writer w(path, 6, kDay1Ns + 60'000 * kMs);
    w.fill(0, kT0 - 300, "207");  // an older trade, found by this session's replay
  }
  const Recovery r = recover(path);
  REQUIRE(r.last_trade_ids.size() == 1);
  CHECK(r.last_trade_ids[0].last_id == 210);
}

// A store an earlier build wrote can hold an execution in two sessions. It is reported; the rows
// stay as they are.
TEST_CASE("store.resume: an execution stored by two sessions is reported as a duplicate") {
  const std::string path = fresh("resume_duplicates.db");
  const std::int64_t minute = 60'000 * kMs;
  {
    Writer w(path, 5, kDay1Ns);
    w.fill(1, kT1, "2966124872");
    w.fill(1, kT1, "2966124873");
  }
  {
    Writer w(path, 6, kDay1Ns + minute);
    w.fill(0, kT0, "2966124872");  // another venue's id that happens to be the same: not a copy
    w.fill(1, kT1 + 60, "2966124877");
  }
  {
    Writer w(path, 7, kDay1Ns + 2 * minute);
    w.fill(1, kT1, "2966124872");
    w.fill(1, kT1, "2966124873");
  }
  {
    Writer w(path, 8, kDay1Ns + 3 * minute);
    w.fill(1, kT1 + 60, "2966124877");
    w.fill(1, kT1, "2966124872");
  }
  const std::int64_t rows = scalar(path, "SELECT COUNT(*) FROM fills");
  const Recovery r = recover(path);
  CHECK(r.duplicate_count == 3);
  REQUIRE(r.duplicates.size() == 3);
  const auto of = [&](std::string_view id) -> const Recovery::Duplicate* {
    for (const Recovery::Duplicate& d : r.duplicates) {
      if (d.id == id) return &d;
    }
    return nullptr;
  };
  REQUIRE(of("2966124872") != nullptr);
  CHECK(of("2966124872")->venue == "bybit");
  CHECK(of("2966124872")->symbol == "ETHUSDT");
  CHECK(of("2966124872")->copies == 3);
  CHECK(of("2966124872")->sessions == "5 7 8");
  CHECK(of("2966124872")->side == "Buy");
  CHECK_FALSE(of("2966124872")->funding);
  REQUIRE(of("2966124873") != nullptr);
  CHECK(of("2966124873")->copies == 2);
  REQUIRE(of("2966124877") != nullptr);
  CHECK(of("2966124877")->sessions == "6 8");
  // Reading changed nothing.
  CHECK(scalar(path, "SELECT COUNT(*) FROM fills") == rows);
  // And each id is listed once for the next replay.
  const Recovery::VenueResume* y = venue(r, "bybit");
  REQUIRE(y != nullptr);
  CHECK(sorted(y->known_exec_ids) ==
        std::vector<std::string>{"2966124872", "2966124873", "2966124877"});

  // The query fastmm-pnl prints, filtered by instrument.
  GenericSection section;
  section.values["path"] = path;
  BackendOptions o;
  o.config = &section;
  o.read_only = true;
  auto reader = make_sqlite_reader();
  REQUIRE(reader->open(o));
  QueryFilter f;
  f.engine = "test";
  auto all = reader->duplicates(f);
  REQUIRE(all);
  CHECK(all->rows.size() == 3);
  f.instrument = "BTCUSDT";
  auto none = reader->duplicates(f);
  REQUIRE(none);
  CHECK(none->empty());
  f.instrument.clear();
  f.engine = "another";
  auto other = reader->duplicates(f);
  REQUIRE(other);
  CHECK(other->empty());
}

// OKX and Binance both number their trades: one session can meet the same id on both.
TEST_CASE("store.resume: the same execution id on two venues of a session keeps both rows") {
  const std::string path = fresh("resume_same_id.db");
  {
    Writer w(path, 5);
    w.fill(0, kT0, "2966124872");
    w.fill(1, kT1, "2966124872");
    w.fill(1, kT1, "2966124872");  // a repeat of the second: one row
  }
  CHECK(scalar(path, "SELECT COUNT(*) FROM fills") == 2);
  CHECK(scalar(path, "SELECT COUNT(DISTINCT venue_id) FROM fills") == 2);
  const Recovery r = recover(path);
  CHECK(r.duplicate_count == 0);
  // Each venue knows its own.
  REQUIRE(venue(r, "binance") != nullptr);
  REQUIRE(venue(r, "bybit") != nullptr);
  CHECK(venue(r, "binance")->known_exec_ids == std::vector<std::string>{"2966124872"});
  CHECK(venue(r, "bybit")->known_exec_ids == std::vector<std::string>{"2966124872"});
}

TEST_CASE("store.resume: a venue with no stored fill replays from the newest clean shutdown") {
  const std::string path = fresh("resume_unbooked.db");
  const std::int64_t start4 = kDay1Ns;
  const std::int64_t start5 = kDay1Ns + 60'000 * kMs;
  const std::int64_t start6 = kDay1Ns + 120'000 * kMs;
  {
    Writer w(path, 4, start4);  // crashed
  }
  {
    Writer w(path, 5, start5);
    w.close_cleanly();
  }
  {
    Writer w(path, 6, start6);  // crashed
  }
  const Recovery r = recover(path);
  CHECK(r.session_id == 6);
  CHECK(r.venue_resume.empty());
  CHECK(r.last_fill_ns == 0);
  CHECK(r.fallback_since_ms == 0);
  CHECK(r.unbooked_since_ms == (start5 - Recovery::kFallbackOverlapNs) / kMs);
}
