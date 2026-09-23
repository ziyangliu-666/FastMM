#include "sqlite_schema.hpp"

#include "fastmm/core/fixed_point.hpp"
#include "fastmm/store/sqlite_store.hpp"
#include "fastmm/version.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace fastmm::store::sqlite {

namespace {

// Version 1: the record tables. Everything a session writes lands here; raw columns are the
// engine's fixed-point integers (1e-8), timestamps are nanoseconds since the Unix epoch, and
// `day` is the UTC day of the timestamp, denormalised so a day query needs no date maths.
constexpr std::string_view kV1 = R"SQL(
CREATE TABLE sessions (
  session_id      INTEGER PRIMARY KEY,
  engine          TEXT    NOT NULL,
  strategy        TEXT    NOT NULL,
  session_epoch   INTEGER NOT NULL,
  started_ns      INTEGER NOT NULL,
  started_day     TEXT    NOT NULL,
  stopped_ns      INTEGER,
  version         TEXT    NOT NULL,
  build           TEXT    NOT NULL,
  config_hash     TEXT    NOT NULL,
  config_toml     TEXT,
  host            TEXT,
  pid             INTEGER,
  dry_run         INTEGER NOT NULL,
  pnl_carry_raw   INTEGER NOT NULL,
  clean_shutdown  INTEGER NOT NULL DEFAULT 0,
  exit_code       INTEGER,
  kill_reason     TEXT,
  kill_latched    INTEGER,
  journal_complete INTEGER,
  journal_bytes   INTEGER,
  events          INTEGER,
  orders_sent     INTEGER,
  cancels_sent    INTEGER,
  replaces_sent   INTEGER,
  fills           INTEGER,
  risk_rejects    INTEGER,
  venue_rejects   INTEGER,
  records_dropped INTEGER,
  realized_raw    INTEGER,
  unrealized_raw  INTEGER,
  fees_raw        INTEGER
);
CREATE INDEX sessions_engine_started ON sessions(engine, started_ns DESC);

CREATE TABLE session_journals (
  session_id INTEGER NOT NULL REFERENCES sessions(session_id),
  part       INTEGER NOT NULL,
  path       TEXT    NOT NULL,
  PRIMARY KEY (session_id, part)
);

CREATE TABLE instruments (
  session_id     INTEGER NOT NULL REFERENCES sessions(session_id),
  instrument_id  INTEGER NOT NULL,
  venue_id       INTEGER NOT NULL,
  symbol         TEXT    NOT NULL,
  base           TEXT    NOT NULL,
  quote          TEXT    NOT NULL,
  settlement_ccy TEXT    NOT NULL,
  asset_class    TEXT    NOT NULL,
  inverse        INTEGER NOT NULL,
  tick_raw       INTEGER NOT NULL,
  lot_raw        INTEGER NOT NULL,
  multiplier_raw INTEGER NOT NULL,
  PRIMARY KEY (session_id, instrument_id)
);

CREATE TABLE fills (
  session_id            INTEGER NOT NULL,
  seq                   INTEGER NOT NULL,
  ts_ns                 INTEGER NOT NULL,
  day                   TEXT    NOT NULL,
  instrument_id         INTEGER NOT NULL,
  venue_id              INTEGER NOT NULL,
  symbol                TEXT    NOT NULL,
  cl_ord_id             TEXT    NOT NULL,
  venue_order_id        TEXT    NOT NULL,
  exec_id               TEXT    NOT NULL,
  side                  TEXT    NOT NULL,
  liquidity             TEXT    NOT NULL,
  price_raw             INTEGER NOT NULL,
  qty_raw               INTEGER NOT NULL,
  booked_qty_raw        INTEGER NOT NULL,
  cum_qty_raw           INTEGER NOT NULL,
  leaves_qty_raw        INTEGER NOT NULL,
  fee_raw               INTEGER NOT NULL,
  fee_amount_raw        INTEGER NOT NULL,
  fee_asset             TEXT    NOT NULL,
  position_qty_raw      INTEGER NOT NULL,
  position_avg_px_raw   INTEGER NOT NULL,
  position_realized_raw INTEGER NOT NULL,
  position_fees_raw     INTEGER NOT NULL,
  synthetic             INTEGER NOT NULL,
  late                  INTEGER NOT NULL,
  PRIMARY KEY (session_id, seq)
);
CREATE INDEX fills_day ON fills(day, symbol);
CREATE INDEX fills_order ON fills(session_id, cl_ord_id);
-- A venue exec id identifies an execution, so re-ingesting one is a no-op. Synthetic fills have
-- no exec id and are excluded.
CREATE UNIQUE INDEX fills_exec ON fills(session_id, exec_id) WHERE exec_id <> '';

CREATE TABLE orders (
  session_id     INTEGER NOT NULL,
  cl_ord_id      TEXT    NOT NULL,
  instrument_id  INTEGER NOT NULL,
  venue_id       INTEGER NOT NULL,
  symbol         TEXT    NOT NULL,
  venue_order_id TEXT    NOT NULL,
  side           TEXT    NOT NULL,
  type           TEXT    NOT NULL,
  tif            TEXT    NOT NULL,
  price_raw      INTEGER NOT NULL,
  qty_raw        INTEGER NOT NULL,
  cum_qty_raw    INTEGER NOT NULL,
  state          TEXT    NOT NULL,
  reject_reason  TEXT    NOT NULL,
  terminal       INTEGER NOT NULL,
  user_tag       INTEGER NOT NULL,
  flags          INTEGER NOT NULL,
  created_ns     INTEGER NOT NULL,
  updated_ns     INTEGER NOT NULL,
  day            TEXT    NOT NULL,
  updates        INTEGER NOT NULL,
  PRIMARY KEY (session_id, cl_ord_id)
);
CREATE INDEX orders_open ON orders(session_id, terminal);
CREATE INDEX orders_day ON orders(day, symbol);

CREATE TABLE positions (
  session_id           INTEGER NOT NULL,
  seq                  INTEGER NOT NULL,
  ts_ns                INTEGER NOT NULL,
  day                  TEXT    NOT NULL,
  instrument_id        INTEGER NOT NULL,
  symbol               TEXT    NOT NULL,
  qty_raw              INTEGER NOT NULL,
  avg_px_raw           INTEGER NOT NULL,
  realized_raw         INTEGER NOT NULL,
  unrealized_raw       INTEGER NOT NULL,
  fees_raw             INTEGER NOT NULL,
  gross_traded_raw     INTEGER NOT NULL,
  fills                INTEGER NOT NULL,
  total_realized_raw   INTEGER NOT NULL,
  total_unrealized_raw INTEGER NOT NULL,
  total_fees_raw       INTEGER NOT NULL,
  pnl_carry_raw        INTEGER NOT NULL,
  PRIMARY KEY (session_id, seq)
);
CREATE INDEX positions_last ON positions(session_id, instrument_id, seq DESC);

CREATE TABLE kill_events (
  session_id     INTEGER NOT NULL,
  seq            INTEGER NOT NULL,
  ts_ns          INTEGER NOT NULL,
  day            TEXT    NOT NULL,
  scope          TEXT    NOT NULL,
  venue_id       INTEGER,
  reason         TEXT    NOT NULL,
  kill_flags     INTEGER NOT NULL,
  realized_raw   INTEGER NOT NULL,
  unrealized_raw INTEGER NOT NULL,
  fees_raw       INTEGER NOT NULL,
  pnl_carry_raw  INTEGER NOT NULL,
  PRIMARY KEY (session_id, seq)
);
)SQL";

// Version 2: the per-day PnL roll-up and the views over it. The roll-up is maintained as position
// records arrive; the backfill below derives it for sessions recorded under version 1, from the
// last position snapshot of each UTC day.
constexpr std::string_view kV2 = R"SQL(
CREATE TABLE pnl_daily (
  session_id       INTEGER NOT NULL,
  day              TEXT    NOT NULL,
  instrument_id    INTEGER NOT NULL,
  symbol           TEXT    NOT NULL,
  settlement_ccy   TEXT    NOT NULL,
  realized_raw     INTEGER NOT NULL DEFAULT 0,
  fees_raw         INTEGER NOT NULL DEFAULT 0,
  unrealized_raw   INTEGER NOT NULL DEFAULT 0,
  qty_raw          INTEGER NOT NULL DEFAULT 0,
  gross_traded_raw INTEGER NOT NULL DEFAULT 0,
  fills            INTEGER NOT NULL DEFAULT 0,
  last_ns          INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (session_id, day, instrument_id)
);
CREATE INDEX pnl_daily_day ON pnl_daily(day);

-- One row per UTC day and instrument across every session.
CREATE VIEW pnl_by_day AS
  SELECT day, symbol, settlement_ccy,
         SUM(realized_raw)     AS realized_raw,
         SUM(fees_raw)         AS fees_raw,
         SUM(realized_raw) - SUM(fees_raw) AS net_raw,
         SUM(gross_traded_raw) AS gross_traded_raw,
         SUM(fills)            AS fills
  FROM pnl_daily
  GROUP BY day, symbol, settlement_ccy;

-- One row per settlement currency and day: what the deployment made that day.
CREATE VIEW pnl_by_currency AS
  SELECT day, settlement_ccy,
         SUM(realized_raw) AS realized_raw,
         SUM(fees_raw)     AS fees_raw,
         SUM(realized_raw) - SUM(fees_raw) AS net_raw,
         SUM(fills)        AS fills
  FROM pnl_daily
  GROUP BY day, settlement_ccy;

-- Orders that were still open when their session recorded its last update.
CREATE VIEW open_orders AS
  SELECT * FROM orders WHERE terminal = 0;

WITH last_per_day AS (
  SELECT session_id, instrument_id, day, MAX(seq) AS seq
  FROM positions GROUP BY session_id, instrument_id, day
),
snap AS (
  SELECT p.session_id, p.instrument_id, p.symbol, p.day, p.ts_ns,
         p.realized_raw, p.fees_raw, p.unrealized_raw, p.qty_raw, p.gross_traded_raw, p.fills
  FROM positions p
  JOIN last_per_day l ON p.session_id = l.session_id
                     AND p.instrument_id = l.instrument_id
                     AND p.seq = l.seq
),
delta AS (
  SELECT s.*,
         LAG(realized_raw, 1, 0)     OVER w AS prev_realized,
         LAG(fees_raw, 1, 0)         OVER w AS prev_fees,
         LAG(gross_traded_raw, 1, 0) OVER w AS prev_gross,
         LAG(fills, 1, 0)            OVER w AS prev_fills
  FROM snap s
  WINDOW w AS (PARTITION BY session_id, instrument_id ORDER BY day)
)
INSERT INTO pnl_daily (session_id, day, instrument_id, symbol, settlement_ccy,
                       realized_raw, fees_raw, unrealized_raw, qty_raw,
                       gross_traded_raw, fills, last_ns)
SELECT d.session_id, d.day, d.instrument_id, d.symbol,
       COALESCE((SELECT i.settlement_ccy FROM instruments i
                 WHERE i.session_id = d.session_id AND i.instrument_id = d.instrument_id), ''),
       d.realized_raw - d.prev_realized,
       d.fees_raw - d.prev_fees,
       d.unrealized_raw,
       d.qty_raw,
       d.gross_traded_raw - d.prev_gross,
       d.fills - d.prev_fills,
       d.ts_ns
FROM delta d;
)SQL";

constexpr std::array<Migration, 2> kMigrations{Migration{1, kV1}, Migration{2, kV2}};

// days since 1970-01-01 -> y/m/d (Howard Hinnant's civil_from_days).
void civil_from_days(std::int64_t z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<unsigned long>(z - era * 146097);
  const unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const auto yy = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned long mp = (5 * doy + 2) / 153;
  d = static_cast<unsigned>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<unsigned>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(yy + (m <= 2 ? 1 : 0));
}

// y/m/d -> days since 1970-01-01 (days_from_civil).
std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
  std::int64_t yy = y;
  yy -= m <= 2 ? 1 : 0;
  const std::int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
  const auto yoe = static_cast<unsigned long>(yy - era * 400);
  const unsigned long doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

constexpr std::int64_t kNsPerDay = 86'400'000'000'000LL;

std::int64_t floor_div(std::int64_t a, std::int64_t b) {
  const std::int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

}  // namespace

std::span<const Migration> migrations() noexcept {
  return {kMigrations.data(), kMigrations.size()};
}

std::string error_of(sqlite3* db, std::string_view what) {
  std::string s(what);
  s += ": ";
  s += sqlite3_errmsg(db);
  return s;
}

Result<void, std::string> exec(sqlite3* db, const char* sql) {
  char* err = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    std::string msg = err == nullptr ? std::string("sqlite error") : std::string(err);
    sqlite3_free(err);
    return fail(msg);
  }
  return {};
}

Result<int, std::string> schema_version(sqlite3* db) {
  sqlite3_stmt* st = nullptr;
  const char* sql = "SELECT version FROM schema_version";
  if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return 0;  // no such table
  int v = 0;
  if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
  sqlite3_finalize(st);
  return v;
}

Result<int, std::string> migrate(sqlite3* db, int target) {
  const auto steps = migrations();
  const int newest = steps.empty() ? 0 : steps.back().version;
  if (target <= 0 || target > newest) target = newest;
  if (auto r = exec(db,
                    "CREATE TABLE IF NOT EXISTS schema_version ("
                    "  version INTEGER NOT NULL,"
                    "  applied_ns INTEGER NOT NULL,"
                    "  fastmm TEXT NOT NULL)");
      !r) {
    return fail(r.error());
  }
  auto cur = schema_version(db);
  if (!cur) return fail(cur.error());
  if (*cur > newest) {
    return fail("the store was written by a newer FastMM (schema version " + std::to_string(*cur) +
                ", this build knows " + std::to_string(newest) +
                "): upgrade FastMM or point [storage] path at another file");
  }
  for (const Migration& m : steps) {
    if (m.version <= *cur || m.version > target) continue;
    if (auto r = exec(db, "BEGIN"); !r) return fail(r.error());
    std::string sql(m.sql);
    if (auto r = exec(db, sql.c_str()); !r) {
      static_cast<void>(exec(db, "ROLLBACK"));
      return fail("schema migration to version " + std::to_string(m.version) +
                  " failed: " + r.error());
    }
    char buf[192];
    std::snprintf(buf,
                  sizeof buf,
                  "DELETE FROM schema_version; INSERT INTO schema_version VALUES (%d, 0, '%s')",
                  m.version,
                  FASTMM_VERSION_STRING);
    if (auto r = exec(db, buf); !r) {
      static_cast<void>(exec(db, "ROLLBACK"));
      return fail(r.error());
    }
    if (auto r = exec(db, "COMMIT"); !r) return fail(r.error());
  }
  auto now = schema_version(db);
  if (!now) return fail(now.error());
  return *now;
}

std::string utc_day(std::int64_t ns) {
  const std::int64_t days = floor_div(ns, kNsPerDay);
  int y = 0;
  unsigned m = 0;
  unsigned d = 0;
  civil_from_days(days, y, m, d);
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", y, m, d);
  return buf;
}

std::string utc_stamp(std::int64_t ns) {
  if (ns == 0) return {};
  const std::int64_t days = floor_div(ns, kNsPerDay);
  std::int64_t rem = ns - days * kNsPerDay;
  int y = 0;
  unsigned m = 0;
  unsigned d = 0;
  civil_from_days(days, y, m, d);
  const std::int64_t secs = rem / 1'000'000'000LL;
  char buf[32];
  std::snprintf(buf,
                sizeof buf,
                "%04d-%02u-%02u %02d:%02d:%02d",
                y,
                m,
                d,
                static_cast<int>(secs / 3600),
                static_cast<int>((secs / 60) % 60),
                static_cast<int>(secs % 60));
  return buf;
}

std::string decimal(std::int64_t raw) {
  char buf[kMaxDecimalChars];
  const std::size_t n = Notional::from_raw(raw).to_decimal(buf);
  return std::string(buf, n);
}

bool day_bounds(std::string_view day, std::int64_t& first_ns, std::int64_t& last_ns) {
  int y = 0;
  unsigned m = 0;
  unsigned d = 0;
  if (day.size() != 10) return false;
  std::string text(day);
  if (std::sscanf(text.c_str(), "%4d-%2u-%2u", &y, &m, &d) != 3) return false;
  if (m < 1 || m > 12 || d < 1 || d > 31) return false;
  const std::int64_t days = days_from_civil(y, m, d);
  first_ns = days * kNsPerDay;
  last_ns = first_ns + kNsPerDay - 1;
  return true;
}

}  // namespace fastmm::store::sqlite
