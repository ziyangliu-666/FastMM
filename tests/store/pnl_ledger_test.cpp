// The PnL a store reports counts each execution once: an estimate an execution named later, an
// execution the engine booked a second time, and a position whose venue had another id in another
// session (all three seen live on Binance: a USD-M account was id 1, then 4, then 5).
#include "../../src/store/sqlite_schema.hpp"
#include "store_test_util.hpp"

#include "fastmm/store/sqlite_store.hpp"

#include <sqlite3.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::store;
using fastmm::test::kDay1Ns;
using fastmm::test::kDay2Ns;
using fastmm::test::tmp_dir;

namespace {

constexpr std::int64_t kOne = 100'000'000;  // 1.0 in raw fixed point

std::string fresh(const char* name) {
  const auto p = tmp_dir() / name;
  std::error_code ec;
  std::filesystem::remove(p, ec);
  std::filesystem::remove(p.string() + "-wal", ec);
  std::filesystem::remove(p.string() + "-shm", ec);
  return p.string();
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
  REQUIRE(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
  const std::int64_t v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
  sqlite3_finalize(st);
  return v;
}

std::string text(sqlite3* db, const char* sql) {
  sqlite3_stmt* st = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
  std::string v;
  if (sqlite3_step(st) == SQLITE_ROW) {
    const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    if (p != nullptr) v = p;
  }
  sqlite3_finalize(st);
  return v;
}

// One instrument's position as the engine keeps it (average cost), writing the records it would:
// a fill, then the position it leaves.
struct Engine {
  Backend& store;
  std::uint64_t session;
  std::uint64_t seq = 0;
  std::int64_t ts = kDay1Ns;
  std::int64_t qty = 0;  // raw
  std::int64_t avg = 0;  // raw
  std::int64_t realized = 0;
  std::int64_t fees = 0;
  std::uint32_t fills = 0;
  std::uint8_t venue = 0;  // the account the order went to

  // Books `moved` of an execution of `qty` at `px`; `named` of it had been booked as an estimate.
  void fill(Side side,
            std::int64_t px,
            std::int64_t q,
            const std::string& exec,
            std::uint32_t order,
            std::int64_t fee = 0,
            bool synthetic = false,
            std::int64_t named = 0,
            std::int64_t est_px = 0) {
    const std::int64_t moved = q - named;
    const std::int64_t d = side == Side::Buy ? moved : -moved;
    if (named > 0) {
      // The estimate is repriced: a sell gains px - est_px on it.
      const std::int64_t diff = (px - est_px) * named / kOne;
      realized += side == Side::Sell ? diff : -diff;
    }
    if (d != 0) {
      if (qty == 0 || (qty > 0) == (d > 0)) {
        const std::int64_t held = qty < 0 ? -qty : qty;
        avg = (avg * held + px * moved) / (held + moved);
        qty += d;
      } else {
        realized += (px - avg) * moved / kOne * (qty > 0 ? 1 : -1);
        qty += d;
        if (qty == 0) avg = 0;
      }
    }
    fees += fee;
    ++fills;
    FillRecord r = fastmm::test::store_fill(session, ++seq, ts, side, px, q, 0, fee, exec);
    r.hdr.venue = VenueId{venue};
    r.cl_ord_id = make_cl_ord_id(7, order);
    r.booked_qty = Qty::from_raw(moved);
    r.named_qty = Qty::from_raw(named);
    if (synthetic) {
      r.hdr.flags |= RecordHeader::kSynthetic;
      r.exec_id.clear();
      r.liquidity = Liquidity::Unknown;
    }
    r.position_qty = Qty::from_raw(qty);
    r.position_avg_px = Price::from_raw(avg);
    r.position_realized = Notional::from_raw(realized);
    r.position_fees = Notional::from_raw(fees);
    store.fill(r);
    PositionRecord p = fastmm::test::store_position(session, ++seq, ts, qty, realized, fees, fills);
    p.pos.avg_px = Price::from_raw(avg);
    store.position(p);
    ts += 1'000'000'000;
  }
};

struct Opened {
  GenericSection section;
  BackendOptions opts;
  std::unique_ptr<Backend> backend;
  explicit Opened(const std::string& path) {
    section.values["path"] = path;
    opts.config = &section;
    opts.engine_name = "test";
    backend = make_sqlite_backend();
    REQUIRE(backend->open(opts));
  }
};

std::unique_ptr<Reader> reader_of(const std::string& path, GenericSection& section) {
  section.values["path"] = path;
  BackendOptions o;
  o.config = &section;
  o.read_only = true;
  auto r = make_sqlite_reader();
  REQUIRE(r->open(o));
  return r;
}

void open_session(Backend& b,
                  std::uint64_t id,
                  std::int64_t started,
                  std::vector<std::string> venues,
                  std::uint8_t instrument_venue) {
  SessionOpen s = fastmm::test::store_session(id, started);
  s.venues = std::move(venues);
  REQUIRE(b.session_open(s));
  InstrumentTable t = fastmm::test::store_table();
  std::vector<Instrument> list(t.data(), t.data() + t.size());
  for (Instrument& i : list) i.venue = VenueId{instrument_venue};
  REQUIRE(b.instruments(id, std::span<const Instrument>(list.data(), list.size())));
}

}  // namespace

// Binance spot: the cancel ack of an order that had traded says executedQty before the fill
// arrives. The engine books the difference at the order's price (an estimate, a synthetic fill)
// and the execution that follows names it. The fills table held both, and anything that summed it
// counted the quantity twice.
TEST_CASE("store.pnl: an estimate an execution named later counts once") {
  const std::string path = fresh("ledger_estimate.db");
  {
    Opened s(path);
    open_session(*s.backend, 1, kDay1Ns, {"binance", "binance_m2"}, 0);
    s.backend->begin();
    Engine e{*s.backend, 1};
    e.fill(Side::Buy, 99 * kOne, kOne, "T1", 1);
    e.venue = 1;                                           // a pool member's order
    e.fill(Side::Sell, 100 * kOne, kOne, "", 2, 0, true);  // the estimate, at the order's price
    e.fill(Side::Sell, 100 * kOne, kOne, "T2", 2, 500, false, kOne, 100 * kOne);  // names it
    // An estimate no execution named (yet).
    e.fill(Side::Buy, 98 * kOne, kOne / 2, "", 3, 0, true);
    s.backend->commit();
    CHECK(s.backend->errors() == 0);
    s.backend->close();
  }
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
  CHECK(scalar(db, "SELECT named_qty_raw FROM fills WHERE seq = 3") == kOne);  // named
  CHECK(scalar(db, "SELECT named_qty_raw FROM fills WHERE seq = 5") == kOne);  // it names
  CHECK(scalar(db, "SELECT named_qty_raw FROM fills WHERE seq = 7") == 0);     // still open
  CHECK(text(db, "SELECT account FROM fills WHERE seq = 5") == "binance_m2");
  CHECK(text(db, "SELECT account FROM positions WHERE seq = 6") == "binance");
  sqlite3_close_v2(db);

  GenericSection section;
  auto reader = reader_of(path, section);
  auto r = reader->pnl(QueryFilter{});
  REQUIRE(r);
  REQUIRE(r->rows.size() == 1);
  const auto& row = r->rows[0];
  CHECK(row[column(*r, "account")] == "binance");
  CHECK(row[column(*r, "symbol")] == "BTCUSDT");
  CHECK(row[column(*r, "realized")] == "1");
  CHECK(row[column(*r, "fees")] == "0.000005");
  CHECK(row[column(*r, "net")] == "0.999995");
  CHECK(row[column(*r, "gross_traded")] == "2.5");  // 1 bought, 1 sold, 0.5 estimated
  CHECK(row[column(*r, "fills")] == "2");
  CHECK(row[column(*r, "estimates")] == "1");
  CHECK(row[column(*r, "books_net")] == "0.999995");  // the engine got this one right
}

// A replay of the venue's trade history re-sent an execution after the OMS had forgotten it: the
// engine booked it again and the store refused the row (fills_exec), so the fills held it once
// while the position and the PnL recorded from then on held it twice.
TEST_CASE("store.pnl: an execution the engine booked twice is a repeat, counted once") {
  const std::string path = fresh("ledger_repeat.db");
  {
    Opened s(path);
    open_session(*s.backend, 1, kDay1Ns, {"binance"}, 0);
    s.backend->begin();
    Engine e{*s.backend, 1};
    e.fill(Side::Buy, 100 * kOne, kOne, "T1", 1);
    e.fill(Side::Sell, 101 * kOne, kOne, "T2", 2);
    e.fill(Side::Buy, 100 * kOne, kOne, "T1", 0);  // T1 again, naming no order
    e.fill(Side::Sell, 102 * kOne, kOne, "T3", 3);
    s.backend->commit();
    CHECK(s.backend->errors() == 0);
    s.backend->close();
  }
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
  CHECK(scalar(db, "SELECT count(*) FROM fills") == 3);
  CHECK(scalar(db, "SELECT count(*) FROM fill_repeats") == 1);
  CHECK(scalar(db, "SELECT seq FROM fill_repeats") == 5);
  CHECK(scalar(db, "SELECT first_seq FROM fill_repeats") == 1);
  CHECK(text(db, "SELECT exec_id FROM fill_repeats") == "T1");
  sqlite3_close_v2(db);

  GenericSection section;
  auto reader = reader_of(path, section);
  auto r = reader->pnl(QueryFilter{});
  REQUIRE(r);
  REQUIRE(r->rows.size() == 1);
  // Bought at 100, sold at 101, then sold short at 102: 1 made, not the 3 the engine booked.
  CHECK(r->rows[0][column(*r, "realized")] == "1");
  CHECK(r->rows[0][column(*r, "fills")] == "3");
  CHECK(r->rows[0][column(*r, "books_net")] == "3");
  auto books = reader->books_pnl(QueryFilter{});
  REQUIRE(books);
  REQUIRE(books->rows.size() == 1);
  CHECK(books->rows[0][column(*books, "net")] == "3");
}

// The venues are numbered by their place in each session's configuration: adding pool members put
// the USD-M account at id 1 in one session and at 3 in the next. The position carries over by name.
TEST_CASE("store.pnl: a position carries across sessions by its account's name, not its id") {
  const std::string path = fresh("ledger_accounts.db");
  {
    Opened s(path);
    open_session(*s.backend, 1, kDay1Ns, {"binance", "binance_usdm"}, 1);
    s.backend->begin();
    Engine a{*s.backend, 1};
    a.venue = 1;
    a.fill(Side::Sell, 50 * kOne, 2 * kOne, "H1", 1);  // a hedge, short 2 at 50
    s.backend->commit();
    s.backend->close();
  }
  {
    Opened s(path);
    open_session(
        *s.backend, 2, kDay2Ns, {"binance", "binance_m2", "binance_m3", "binance_usdm"}, 3);
    s.backend->begin();
    Engine b{*s.backend, 2};
    b.venue = 3;
    b.ts = kDay2Ns + 1'000'000'000;
    b.qty = -2 * kOne;  // restored from the store
    b.avg = 50 * kOne;
    b.fill(Side::Buy, 48 * kOne, 2 * kOne, "H2", 1);
    // Spot on the pool's primary, id 0 in both sessions.
    s.backend->commit();
    s.backend->close();
  }
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
  CHECK(scalar(db, "SELECT count(DISTINCT venue_id) FROM fills") == 2);
  CHECK(scalar(db, "SELECT count(DISTINCT account) FROM fills") == 1);
  CHECK(text(db, "SELECT account FROM fills WHERE session_id = 2") == "binance_usdm");
  CHECK(text(db, "SELECT account FROM pnl_daily WHERE session_id = 1") == "binance_usdm");
  sqlite3_close_v2(db);

  GenericSection section;
  auto reader = reader_of(path, section);
  auto r = reader->pnl(QueryFilter{});
  REQUIRE(r);
  REQUIRE(r->rows.size() == 2);
  CHECK(r->rows[0][column(*r, "account")] == "binance_usdm");
  CHECK(r->rows[0][column(*r, "realized")] == "0");
  CHECK(r->rows[1][column(*r, "account")] == "binance_usdm");
  CHECK(r->rows[1][column(*r, "realized")] == "4");  // covered 2 at 48 against 50
  auto books = reader->books_pnl(QueryFilter{});
  REQUIRE(books);
  REQUIRE(books->rows.size() == 2);
  CHECK(books->rows[1][column(*books, "account")] == "binance_usdm");
}

// A store written before version 10 is backfilled: every row's account, the estimates executions
// named, and the repeats its position records show.
TEST_CASE("store.pnl: a version 9 store gains accounts, named estimates and its repeats") {
  const std::string path = fresh("ledger_v9.db");
  {
    sqlite3* db = nullptr;
    REQUIRE(
        sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) ==
        SQLITE_OK);
    REQUIRE(sqlite::migrate(db, 9));
    const char* sql = R"SQL(
INSERT INTO sessions (session_id, engine, strategy, session_epoch, started_ns, started_day,
  version, build, config_hash, dry_run, pnl_carry_raw) VALUES (1,'test','x',1,0,'2024-03-04',
  '0.5.1','b','0',0,0);
INSERT INTO session_venues VALUES (1,0,'binance',0),(1,1,'binance_m2',0);
INSERT INTO instruments VALUES (1,0,0,'BTCUSDT','BTC','USDT','USDT','Spot',0,1000000,100000,
  100000000);
INSERT INTO fills (session_id, seq, ts_ns, day, instrument_id, venue_id, symbol, cl_ord_id,
  venue_order_id, exec_id, side, liquidity, price_raw, qty_raw, booked_qty_raw, cum_qty_raw,
  leaves_qty_raw, fee_raw, fee_amount_raw, fee_asset, position_qty_raw, position_avg_px_raw,
  position_realized_raw, position_fees_raw, synthetic, late, exch_ns) VALUES
 (1,1,10,'2024-03-04',0,0,'BTCUSDT','a','v','T1','Buy','Maker',10000000000,100000000,100000000,
  0,0,0,0,'quote',100000000,10000000000,0,0,0,0,10),
 (1,3,20,'2024-03-04',0,1,'BTCUSDT','b','v','','Sell','Unknown',10100000000,100000000,100000000,
  0,0,0,0,'quote',0,0,100000000,0,1,0,0),
 (1,5,21,'2024-03-04',0,1,'BTCUSDT','b','v','T2','Sell','Maker',10100000000,100000000,0,
  0,0,0,0,'quote',0,0,100000000,0,0,1,21);
INSERT INTO positions (session_id, seq, ts_ns, day, instrument_id, symbol, qty_raw, avg_px_raw,
  realized_raw, unrealized_raw, fees_raw, gross_traded_raw, fills, total_realized_raw,
  total_unrealized_raw, total_fees_raw, pnl_carry_raw) VALUES
 (1,2,10,'2024-03-04',0,'BTCUSDT',100000000,10000000000,0,0,0,0,1,0,0,0,0),
 (1,4,20,'2024-03-04',0,'BTCUSDT',0,0,100000000,0,0,0,2,0,0,0,0),
 (1,6,21,'2024-03-04',0,'BTCUSDT',0,0,100000000,0,0,0,3,0,0,0,0),
 (1,8,30,'2024-03-04',0,'BTCUSDT',100000000,10000000000,100000000,0,0,0,4,0,0,0,0);
)SQL";
    REQUIRE(sqlite::exec(db, sql));
    sqlite3_close_v2(db);
  }
  {
    Opened s(path);  // a session's writer migrates it
    s.backend->close();
  }
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
  CHECK(scalar(db, "SELECT version FROM schema_version") == kSqliteSchemaVersion);
  CHECK(text(db, "SELECT account FROM fills WHERE seq = 3") == "binance_m2");
  CHECK(text(db, "SELECT account FROM positions WHERE seq = 2") == "binance");
  CHECK(scalar(db, "SELECT named_qty_raw FROM fills WHERE seq = 3") == kOne);
  CHECK(scalar(db, "SELECT named_qty_raw FROM fills WHERE seq = 5") == kOne);
  CHECK(scalar(db, "SELECT named_qty_raw FROM fills WHERE seq = 1") == 0);
  // seq 8: the position rose by 1 at 100 with no fill before it: T1 again, the record at seq 7.
  CHECK(scalar(db, "SELECT count(*) FROM fill_repeats") == 1);
  CHECK(text(db, "SELECT exec_id FROM fill_repeats WHERE seq = 7") == "T1");
  CHECK(scalar(db, "SELECT first_seq FROM fill_repeats") == 1);
  sqlite3_close_v2(db);

  GenericSection section;
  auto reader = reader_of(path, section);
  auto r = reader->pnl(QueryFilter{});
  REQUIRE(r);
  REQUIRE(r->rows.size() == 1);
  CHECK(r->rows[0][column(*r, "realized")] == "1");
  CHECK(r->rows[0][column(*r, "fills")] == "2");
  CHECK(r->rows[0][column(*r, "estimates")] == "0");
}
