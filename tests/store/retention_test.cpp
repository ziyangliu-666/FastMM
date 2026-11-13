// [storage] retention_days: the SQLite backend deletes old orders, position snapshots, markouts
// and rejects in chunks when the store thread is idle, and leaves what a restart reads: open
// orders, each session's last position of an instrument, the previous session whole, the orders of
// a session a restart judges by them, and every fill.
#include "store_test_util.hpp"

#include "fastmm/store/sqlite_store.hpp"

#include <sqlite3.h>

#include <filesystem>
#include <memory>
#include <string>

using namespace fastmm;
using namespace fastmm::store;
using fastmm::test::kDay1Ns;
using fastmm::test::tmp_dir;

namespace {

constexpr std::int64_t kDayNs = 86'400'000'000'000LL;
constexpr std::int64_t kHourNs = 3'600'000'000'000LL;

std::int64_t day(int n) {
  return kDay1Ns + n * kDayNs;
}

std::filesystem::path fresh(const char* name) {
  const auto p = tmp_dir() / name;
  std::error_code ec;
  std::filesystem::remove(p, ec);
  std::filesystem::remove(p.string() + "-wal", ec);
  std::filesystem::remove(p.string() + "-shm", ec);
  return p;
}

std::unique_ptr<Backend> open_store(const std::filesystem::path& p,
                                    const char* retention,
                                    GenericSection& section) {
  section.values["path"] = p.string();
  if (retention != nullptr) section.values["retention_days"] = retention;
  BackendOptions o;
  o.config = &section;
  auto b = make_sqlite_backend();
  REQUIRE(b->open(o));
  return b;
}

PositionRecord position(std::uint64_t session,
                        std::uint64_t seq,
                        std::int64_t ts,
                        std::uint32_t i) {
  PositionRecord r = fastmm::test::store_position(
      session, seq, ts, 1000 + static_cast<std::int64_t>(seq), 0, 0, 1);
  r.hdr.instrument = InstrumentId{i};
  return r;
}

MarkoutRecord markout(std::uint64_t session, std::uint64_t fill_seq, std::int64_t fill_ts, int h) {
  MarkoutRecord r{};
  r.hdr.len = sizeof r;
  r.hdr.type = RecordType::Markout;
  r.hdr.session_id = session;
  r.hdr.instrument = InstrumentId{0};
  r.hdr.engine_ts = Timestamp{fill_ts + h * 1'000'000LL};
  r.fill_seq = fill_seq;
  r.fill_ts = Timestamp{fill_ts};
  r.horizon_ms = h;
  r.mid = Price::from_decimal("100").value();
  return r;
}

RejectRecord reject(std::uint64_t session, std::uint64_t seq, std::int64_t ts) {
  RejectRecord r{};
  r.hdr.len = sizeof r;
  r.hdr.type = RecordType::Reject;
  r.hdr.session_id = session;
  r.hdr.seq = seq;
  r.hdr.engine_ts = Timestamp{ts};
  r.hdr.instrument = InstrumentId{0};
  r.side = Side::Buy;
  r.reason = RejectReason::RateLimit;
  r.source = RejectSource::RiskBucket;
  return r;
}

// One session of `engine` that started on `start`: an order that ended and one still open, three
// position snapshots (two of BTCUSDT, one of ETHUSDT), a fill with two markouts, and `rejects`
// refusals, all in its first hours. The caller closes it, or not.
void old_session(Backend& b,
                 std::uint64_t id,
                 const char* engine,
                 std::int64_t start,
                 const InstrumentTable& table,
                 int rejects = 1) {
  SessionOpen s = fastmm::test::store_session(id, start);
  s.engine_name = engine;
  s.venues = {"binance"};
  REQUIRE(b.session_open(s));
  REQUIRE(b.instruments(id, std::span<const Instrument>(table.data(), table.size())));
  const std::int64_t t = start + kHourNs;
  b.begin();
  b.order(fastmm::test::store_order(id, 1, t, 1, OrderState::Filled));
  b.order(fastmm::test::store_order(id, 2, t, 2, OrderState::Live));
  b.fill(fastmm::test::store_fill(id, 3, t, Side::Buy, 10'000'000'000, 100'000'000, 0, 0, "E1"));
  b.position(position(id, 4, t, 0));
  b.position(position(id, 5, t + kHourNs, 0));
  b.position(position(id, 6, t, 1));
  b.markout(markout(id, 3, t, 0));
  b.markout(markout(id, 3, t, 1000));
  for (int k = 0; k < rejects; ++k)
    b.reject(reject(id, 100 + static_cast<std::uint64_t>(k), t + k));
  b.commit();
}

void close_session(Backend& b, std::uint64_t id, std::int64_t stopped) {
  SessionClose c;
  c.session_id = id;
  c.stopped_ns = stopped;
  REQUIRE(b.session_close(c));
}

std::int64_t count(sqlite3* db, const std::string& sql) {
  sqlite3_stmt* st = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(st) == SQLITE_ROW);
  const std::int64_t n = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return n;
}

std::int64_t rows(sqlite3* db, const char* table, std::uint64_t session) {
  return count(db,
               std::string("SELECT COUNT(*) FROM ") + table +
                   " WHERE session_id = " + std::to_string(session));
}

}  // namespace

TEST_CASE("store.retention: old rows go in chunks, what a restart reads stays") {
  const auto p = fresh("retention.db");
  const InstrumentTable table = fastmm::test::store_table();
  GenericSection section;
  auto b = open_store(p, "7", section);

  // 10: crashed before schema 6's replayed_seq (set NULL below), so a restart judges it by its
  // orders. 11: an ordinary old session, with 2500 refusals (three chunks). 12: the previous
  // session of "test". 14: the only session of "other". 13: this session, from day 3 to day 20.
  old_session(*b, 10, "test", day(0), table);
  old_session(*b, 11, "test", day(1), table, 2500);
  close_session(*b, 11, day(1) + 5 * kHourNs);
  old_session(*b, 14, "other", day(2), table);
  close_session(*b, 14, day(2) + 5 * kHourNs);
  old_session(*b, 12, "test", day(2) + kHourNs, table);
  close_session(*b, 12, day(2) + 5 * kHourNs);
  old_session(*b, 13, "test", day(3), table);
  b->begin();
  // An order sent on day 3 that ended on day 19, one that ended on day 20, a new snapshot of
  // BTCUSDT, and a new fill with its markouts and a refusal.
  b->order(fastmm::test::store_order(13, 200, day(3) + kHourNs, 3, OrderState::Live));
  OrderRecord late = fastmm::test::store_order(13, 201, day(19), 3, OrderState::Canceled);
  late.order.created = Timestamp{day(3) + kHourNs};
  b->order(late);
  b->order(fastmm::test::store_order(13, 202, day(20), 4, OrderState::Filled));
  b->fill(fastmm::test::store_fill(13, 203, day(20), Side::Sell, 10'000'000'000, 1, 0, 0, "E2"));
  b->position(position(13, 204, day(20), 0));
  b->markout(markout(13, 203, day(20), 0));
  b->markout(markout(13, 203, day(20), 1000));
  b->reject(reject(13, 205, day(20)));
  b->commit();

  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open(p.string().c_str(), &db) == SQLITE_OK);
  REQUIRE(sqlite3_exec(db,
                       "UPDATE session_venues SET replayed_seq = NULL WHERE session_id = 10",
                       nullptr,
                       nullptr,
                       nullptr) == SQLITE_OK);
  const std::int64_t before = count(db, "SELECT COUNT(*) FROM rejects");

  // Nothing in the first minute after the first idle moment.
  const std::int64_t now = day(20) + kHourNs;
  b->idle(now);
  b->idle(now + 59'000'000'000LL);
  CHECK(count(db, "SELECT COUNT(*) FROM rejects") == before);

  // Then one chunk per call, 50 ms apart.
  int calls = 0;
  for (std::int64_t t = now + 60'000'000'000LL; calls < 100; t += 50'000'000, ++calls) {
    b->idle(t);
    if (rows(db, "rejects", 11) > 0 && rows(db, "rejects", 11) < 2500) {
      // A chunk reads 1000 rows: the refusals of session 11 are gone in steps.
      CHECK(rows(db, "rejects", 11) % 1000 == 500);
    }
  }

  // Session 10: its orders stay (a restart may need them), its other old rows go but for the
  // last snapshot of each instrument.
  CHECK(rows(db, "orders", 10) == 2);
  CHECK(rows(db, "positions", 10) == 2);
  CHECK(count(db, "SELECT COUNT(*) FROM positions WHERE session_id = 10 AND seq = 4") == 0);
  CHECK(rows(db, "markouts", 10) == 0);
  CHECK(rows(db, "rejects", 10) == 0);
  // Session 11: the open order stays.
  CHECK(rows(db, "orders", 11) == 1);
  CHECK(count(db, "SELECT COUNT(*) FROM orders WHERE session_id = 11 AND terminal = 0") == 1);
  CHECK(rows(db, "positions", 11) == 2);
  CHECK(rows(db, "markouts", 11) == 0);
  CHECK(rows(db, "rejects", 11) == 0);
  // Sessions 12 and 14, the newest of their engines but this one, stay whole.
  for (const std::uint64_t s : {12U, 14U}) {
    CAPTURE(s);
    CHECK(rows(db, "orders", s) == 2);
    CHECK(rows(db, "positions", s) == 3);
    CHECK(rows(db, "markouts", s) == 2);
    CHECK(rows(db, "rejects", s) == 1);
  }
  // This session: the order that ended on day 3 goes; the open one, the one that ended on day 19
  // and the new one stay. BTCUSDT's old snapshots go (its last is new), ETHUSDT's last stays.
  CHECK(rows(db, "orders", 13) == 3);
  CHECK(count(db, "SELECT COUNT(*) FROM orders WHERE session_id = 13 AND state = 'Filled'") == 1);
  CHECK(rows(db, "positions", 13) == 2);
  CHECK(count(db, "SELECT COUNT(*) FROM positions WHERE session_id = 13 AND seq IN (6, 204)") == 2);
  CHECK(rows(db, "markouts", 13) == 2);
  CHECK(rows(db, "rejects", 13) == 1);
  // Every fill stays, and so do the day roll-up and the sessions.
  CHECK(count(db, "SELECT COUNT(*) FROM fills") == 6);
  CHECK(count(db, "SELECT COUNT(*) FROM sessions") == 5);
  CHECK(count(db, "SELECT COUNT(DISTINCT session_id) FROM pnl_daily") == 5);
  sqlite3_close(db);

  // A restart finds the same position and open orders as without the retention.
  b->close();
  BackendOptions ro;
  ro.config = &section;
  ro.read_only = true;
  auto reader = make_sqlite_reader();
  REQUIRE(reader->open(ro));
  QueryFilter f;
  f.engine = "test";
  auto rec = reader->recovery(f);
  REQUIRE(rec);
  CHECK(rec->session_id == 13);
  CHECK(rec->open_orders.size() == 1);
  REQUIRE(rec->position_state.size() == 2);
  for (const auto& ps : rec->position_state) {
    CAPTURE(ps.symbol);
    CHECK(ps.qty_raw == (ps.symbol == "BTCUSDT" ? 1000 + 204 : 1000 + 6));
  }
}

TEST_CASE("store.retention: off by default, and a negative value is refused") {
  const auto p = fresh("retention_off.db");
  const InstrumentTable table = fastmm::test::store_table();
  GenericSection section;
  auto b = open_store(p, nullptr, section);
  old_session(*b, 1, "test", day(0), table);
  close_session(*b, 1, day(0) + 5 * kHourNs);
  old_session(*b, 2, "test", day(1), table);
  for (int k = 0; k < 10; ++k) b->idle(day(30) + k * kHourNs);
  b->close();
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open(p.string().c_str(), &db) == SQLITE_OK);
  CHECK(rows(db, "orders", 1) == 2);
  CHECK(rows(db, "positions", 1) == 3);
  CHECK(rows(db, "markouts", 1) == 2);
  CHECK(rows(db, "rejects", 1) == 1);
  sqlite3_close(db);

  GenericSection bad;
  bad.values["path"] = fresh("retention_bad.db").string();
  bad.values["retention_days"] = "-1";
  BackendOptions o;
  o.config = &bad;
  auto refused = make_sqlite_backend();
  const auto r = refused->open(o);
  REQUIRE_FALSE(r);
  CHECK(r.error().find("retention_days") != std::string::npos);
}
