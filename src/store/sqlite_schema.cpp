#include "sqlite_schema.hpp"

#include "fastmm/core/fixed_point.hpp"
#include "fastmm/store/sqlite_store.hpp"
#include "fastmm/strategies/params.hpp"
#include "fastmm/version.hpp"

#include <array>
#include <bit>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

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
-- no exec id and are excluded. (Version 5 keys it by instrument and side as well.)
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

// Version 3: the venue's time of each fill and the names of a session's venues, so a restart can
// resume the venue's execution replay in the venue's clock (Recovery::venue_resume). Rows written
// before it have exch_ns = 0 and their sessions no venue names; a restart then falls back to the
// engine clock, as version 2 did.
constexpr std::string_view kV3 = R"SQL(
ALTER TABLE fills ADD COLUMN exch_ns INTEGER NOT NULL DEFAULT 0;
CREATE INDEX fills_venue_time ON fills(session_id, venue_id, exch_ns);

CREATE TABLE session_venues (
  session_id INTEGER NOT NULL,
  venue_id   INTEGER NOT NULL,
  name       TEXT    NOT NULL,
  PRIMARY KEY (session_id, venue_id)
);
)SQL";

// Version 4: funding payments on perpetuals. One row per payment the engine booked; funding is
// part of realized PnL, and the positions, the day roll-up, its views and the session totals say
// how much of it. Rows written before it have funding 0 (nothing was booked).
constexpr std::string_view kV4 = R"SQL(
CREATE TABLE funding (
  session_id            INTEGER NOT NULL,
  seq                   INTEGER NOT NULL,
  ts_ns                 INTEGER NOT NULL,
  day                   TEXT    NOT NULL,
  exch_ns               INTEGER NOT NULL,
  instrument_id         INTEGER NOT NULL,
  venue_id              INTEGER NOT NULL,
  symbol                TEXT    NOT NULL,
  funding_id            TEXT    NOT NULL,
  asset                 TEXT    NOT NULL,
  amount_raw            INTEGER NOT NULL,
  position_qty_raw      INTEGER NOT NULL,
  position_realized_raw INTEGER NOT NULL,
  position_funding_raw  INTEGER NOT NULL,
  replayed              INTEGER NOT NULL,
  PRIMARY KEY (session_id, seq)
);
CREATE INDEX funding_day ON funding(day, symbol);
CREATE INDEX funding_venue_time ON funding(session_id, venue_id, exch_ns);
CREATE UNIQUE INDEX funding_venue_id ON funding(session_id, instrument_id, funding_id)
  WHERE funding_id <> '';

ALTER TABLE positions ADD COLUMN funding_raw INTEGER NOT NULL DEFAULT 0;
ALTER TABLE positions ADD COLUMN total_funding_raw INTEGER NOT NULL DEFAULT 0;
ALTER TABLE pnl_daily ADD COLUMN funding_raw INTEGER NOT NULL DEFAULT 0;
ALTER TABLE sessions ADD COLUMN funding_raw INTEGER;

DROP VIEW pnl_by_day;
CREATE VIEW pnl_by_day AS
  SELECT day, symbol, settlement_ccy,
         SUM(realized_raw)     AS realized_raw,
         SUM(funding_raw)      AS funding_raw,
         SUM(fees_raw)         AS fees_raw,
         SUM(realized_raw) - SUM(fees_raw) AS net_raw,
         SUM(gross_traded_raw) AS gross_traded_raw,
         SUM(fills)            AS fills
  FROM pnl_daily
  GROUP BY day, symbol, settlement_ccy;

DROP VIEW pnl_by_currency;
CREATE VIEW pnl_by_currency AS
  SELECT day, settlement_ccy,
         SUM(realized_raw) AS realized_raw,
         SUM(funding_raw)  AS funding_raw,
         SUM(fees_raw)     AS fees_raw,
         SUM(realized_raw) - SUM(fees_raw) AS net_raw,
         SUM(fills)        AS fills
  FROM pnl_daily
  GROUP BY day, settlement_ccy;
)SQL";

// Version 5: an execution is the venue's id of it on one instrument and side, as the OMS keys it.
// The index was on (session_id, exec_id): two venues of a session that number their trades alike
// (OKX and Binance both count), two symbols of a venue whose ids run per symbol (Binance), and the
// two halves of a self-trade shared one row - the second was dropped as a repeat. Rows an older
// store dropped that way are not in it; the journal has them.
constexpr std::string_view kV5 = R"SQL(
DROP INDEX fills_exec;
CREATE UNIQUE INDEX fills_exec ON fills(session_id, instrument_id, side, exec_id)
  WHERE exec_id <> '';
)SQL";

// Version 6: whether a session got past its execution replay on a venue. replayed_seq is the
// record seq of the last reconciliation of that venue that began with the venue's executions
// complete; 0 while there has been none; NULL for a session recorded before this version, which a
// restart judges by other signs (Recovery). A session without one may hold a fill newer than one
// its replay had not reached, and the next replay starts before its fills.
constexpr std::string_view kV6 = R"SQL(
ALTER TABLE session_venues ADD COLUMN replayed_seq INTEGER;
)SQL";

// Version 7: the venue's ids in id order, so the check for an execution or a funding payment
// stored by more than one session (Reader::duplicates, run at every start) walks these two indexes
// instead of grouping the tables: the ids that occur once are passed over without touching a row.
constexpr std::string_view kV7 = R"SQL(
CREATE INDEX fills_exec_any ON fills(exec_id, symbol, side) WHERE exec_id <> '';
CREATE INDEX funding_id_any ON funding(funding_id, symbol) WHERE funding_id <> '';
)SQL";

// Version 8: strategy parameters over time. param_schema names a session's parameters (a
// ParamUpdate's field index); param_updates has one row per update the engine applied, and seq 0
// the session's starting set (origin 'initial'); param_values the values each carried, `changed`
// against the value before it. A strategy's own publisher refreshing values that did not change
// writes no row: sessions.param_refreshes counts those.
constexpr std::string_view kV8 = R"SQL(
CREATE TABLE param_schema (
  session_id INTEGER NOT NULL,
  field      INTEGER NOT NULL,
  name       TEXT    NOT NULL,
  type       TEXT    NOT NULL,
  PRIMARY KEY (session_id, field)
);

CREATE TABLE param_updates (
  session_id    INTEGER NOT NULL,
  seq           INTEGER NOT NULL,
  ts_ns         INTEGER NOT NULL,
  day           TEXT    NOT NULL,
  instrument_id INTEGER,
  symbol        TEXT,
  origin        TEXT    NOT NULL,
  source        TEXT    NOT NULL,
  publish_seq   INTEGER NOT NULL,
  fields        INTEGER NOT NULL,
  changed       INTEGER NOT NULL,
  complete      INTEGER NOT NULL,
  PRIMARY KEY (session_id, seq)
);
CREATE INDEX param_updates_time ON param_updates(ts_ns);

CREATE TABLE param_values (
  session_id INTEGER NOT NULL,
  seq        INTEGER NOT NULL,
  name       TEXT    NOT NULL,
  value      TEXT    NOT NULL,
  raw        INTEGER,
  changed    INTEGER NOT NULL,
  PRIMARY KEY (session_id, seq, name)
);
CREATE INDEX param_values_name ON param_values(name, session_id, seq);

ALTER TABLE sessions ADD COLUMN param_refreshes INTEGER;

-- Every value every update carried, newest last.
CREATE VIEW param_history AS
  SELECT u.session_id, u.seq, u.ts_ns, u.day, u.instrument_id, u.symbol, u.origin, u.source,
         u.publish_seq, v.name, v.value, v.changed
  FROM param_updates u JOIN param_values v ON v.session_id = u.session_id AND v.seq = u.seq;
)SQL";

// Version 9: refused orders (RejectRecord). One row per refusal the engine recorded; `count` is
// 1 plus the refusals of the same instrument, side, reason and source folded into it (within
// 100 ms of the row before), so SUM(count) is exact. `source` says which limit or side refused
// it; the budget columns are the account's as the engine saw it then (ctx.order_budget): the
// [risk] bucket's tokens (NULL: no limit) and each order window's count against what the
// connector admits (0: the window is not known).
constexpr std::string_view kV9 = R"SQL(
CREATE TABLE rejects (
  session_id        INTEGER NOT NULL,
  seq               INTEGER NOT NULL,
  ts_ns             INTEGER NOT NULL,
  day               TEXT    NOT NULL,
  account           TEXT    NOT NULL,
  venue_id          INTEGER NOT NULL,
  instrument_id     INTEGER NOT NULL,
  symbol            TEXT    NOT NULL,
  side              TEXT    NOT NULL,
  kind              TEXT    NOT NULL,
  reduces           INTEGER NOT NULL,
  reduce_only       INTEGER NOT NULL,
  reason            TEXT    NOT NULL,
  source            TEXT    NOT NULL,
  count             INTEGER NOT NULL,
  price_raw         INTEGER NOT NULL,
  qty_raw           INTEGER NOT NULL,
  cl_ord_id         TEXT    NOT NULL,
  venue_code        INTEGER NOT NULL,
  text              TEXT    NOT NULL,
  budget_known      INTEGER NOT NULL,
  paused            INTEGER NOT NULL,
  tokens            INTEGER,
  token_wait_us     INTEGER NOT NULL,
  orders_10s_used   INTEGER NOT NULL,
  orders_10s_admits INTEGER NOT NULL,
  orders_1m_used    INTEGER NOT NULL,
  orders_1m_admits  INTEGER NOT NULL,
  orders_1d_used    INTEGER NOT NULL,
  orders_1d_admits  INTEGER NOT NULL,
  weight_used       INTEGER NOT NULL,
  weight_admits     INTEGER NOT NULL,
  PRIMARY KEY (session_id, seq)
);
CREATE INDEX rejects_day ON rejects(day, account, source);

-- Who refused what: one row per day, account, instrument, side, whether the order only reduced
-- the position, reason and source.
CREATE VIEW reject_summary AS
  SELECT day, account, symbol, side, reduces, reason, source, SUM(count) AS rejects
  FROM rejects
  GROUP BY day, account, symbol, side, reduces, reason, source;
)SQL";

// Version 10: who holds what, by name, and which fills are not executions.
//
// `account` is the [venues.<name>] of a row's venue in the session that wrote it. venue_id is the
// venue's place in that session's configuration, so the same id named another account in another
// session (a venue added to a pool moved the USD-M account from id 1 to 4, then 5): rows of
// several sessions group by the name. A fill's account is the one it executed on (a pool member);
// an instrument's, a position's and a day roll-up's is the instrument's own venue (a pool's
// primary), which holds the position.
//
// `named_qty_raw` says how much of a fill is not an execution of its own. A synthetic fill is an
// estimate the engine booked from a cum_qty no execution had reported yet (a cancel ack overtaking
// the fill on Binance spot); on it, named_qty_raw is how much of it executions named since. On an
// execution, it is how much of it named an estimate: the position held that part already, so the
// execution moved it by booked_qty only and corrected the estimate's price and fee. Counting
// executions whole and estimates for qty_raw - named_qty_raw counts each traded unit once.
// Rows written before this version are backfilled from booked_qty (an execution of an order with an
// estimate that booked less than its quantity), the estimates oldest first.
//
// fill_repeats: an execution the engine booked a second time in a session that had stored it (its
// row was refused by fills_exec), at the seq of the fill record the store refused (the position
// record after it is seq + 1). The position and the PnL the engine recorded from then on count it
// twice; the fills do not. Older sessions are backfilled from their position records: a change
// of position with no fill or funding before it that one stored execution explains (same side and
// quantity, price within a tick of the one the position's change implies).
constexpr std::string_view kV10 = R"SQL(
ALTER TABLE fills ADD COLUMN account TEXT NOT NULL DEFAULT '';
ALTER TABLE fills ADD COLUMN named_qty_raw INTEGER NOT NULL DEFAULT 0;
ALTER TABLE funding ADD COLUMN account TEXT NOT NULL DEFAULT '';
ALTER TABLE instruments ADD COLUMN account TEXT NOT NULL DEFAULT '';
ALTER TABLE positions ADD COLUMN account TEXT NOT NULL DEFAULT '';
ALTER TABLE pnl_daily ADD COLUMN account TEXT NOT NULL DEFAULT '';

UPDATE fills SET account = COALESCE((SELECT v.name FROM session_venues v
  WHERE v.session_id = fills.session_id AND v.venue_id = fills.venue_id), '#' || venue_id);
UPDATE funding SET account = COALESCE((SELECT v.name FROM session_venues v
  WHERE v.session_id = funding.session_id AND v.venue_id = funding.venue_id), '#' || venue_id);
UPDATE instruments SET account = COALESCE((SELECT v.name FROM session_venues v
  WHERE v.session_id = instruments.session_id AND v.venue_id = instruments.venue_id),
  '#' || venue_id);
UPDATE positions SET account = COALESCE((SELECT i.account FROM instruments i
  WHERE i.session_id = positions.session_id AND i.instrument_id = positions.instrument_id), '');
UPDATE pnl_daily SET account = COALESCE((SELECT i.account FROM instruments i
  WHERE i.session_id = pnl_daily.session_id AND i.instrument_id = pnl_daily.instrument_id), '');
CREATE INDEX fills_account ON fills(account, symbol, ts_ns);

UPDATE fills SET named_qty_raw = MAX(0,
    CASE WHEN fee_asset = 'base' AND side = 'Buy' THEN qty_raw - fee_amount_raw
         WHEN fee_asset = 'base' THEN qty_raw + fee_amount_raw
         ELSE qty_raw END - booked_qty_raw)
  WHERE synthetic = 0 AND EXISTS (SELECT 1 FROM fills e WHERE e.session_id = fills.session_id
    AND e.cl_ord_id = fills.cl_ord_id AND e.synthetic <> 0);
WITH named AS (
  SELECT session_id, cl_ord_id, SUM(named_qty_raw) AS qty FROM fills
  WHERE synthetic = 0 AND named_qty_raw > 0 GROUP BY session_id, cl_ord_id),
est AS (
  SELECT f.session_id, f.seq, f.qty_raw, n.qty AS named,
         COALESCE(SUM(f.qty_raw) OVER (PARTITION BY f.session_id, f.cl_ord_id ORDER BY f.seq
                  ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING), 0) AS before
  FROM fills f JOIN named n ON n.session_id = f.session_id AND n.cl_ord_id = f.cl_ord_id
  WHERE f.synthetic <> 0)
UPDATE fills SET named_qty_raw = MAX(0, MIN(est.qty_raw, est.named - est.before))
  FROM est WHERE fills.session_id = est.session_id AND fills.seq = est.seq;

CREATE TABLE fill_repeats (
  session_id     INTEGER NOT NULL,
  seq            INTEGER NOT NULL,
  ts_ns          INTEGER NOT NULL,
  day            TEXT    NOT NULL,
  account        TEXT    NOT NULL,
  instrument_id  INTEGER NOT NULL,
  symbol         TEXT    NOT NULL,
  side           TEXT    NOT NULL,
  exec_id        TEXT    NOT NULL,
  price_raw      INTEGER NOT NULL,
  qty_raw        INTEGER NOT NULL,
  booked_qty_raw INTEGER NOT NULL,
  first_seq      INTEGER,
  PRIMARY KEY (session_id, seq)
);
CREATE INDEX fill_repeats_day ON fill_repeats(day, symbol);

WITH p AS (
  SELECT session_id, seq, ts_ns, day, instrument_id, symbol, account, qty_raw, avg_px_raw,
         realized_raw,
         LAG(qty_raw) OVER w AS prev_qty, LAG(avg_px_raw) OVER w AS prev_avg,
         LAG(realized_raw) OVER w AS prev_realized
  FROM positions WINDOW w AS (PARTITION BY session_id, instrument_id ORDER BY seq)),
jump AS (
  SELECT p.*, CASE WHEN qty_raw > prev_qty THEN 'Buy' ELSE 'Sell' END AS side,
         ABS(qty_raw - prev_qty) AS moved,
         -- The price the change implies: an increase moves the average, a decrease realizes.
         CASE WHEN prev_qty = 0 OR (prev_qty > 0 AND qty_raw > prev_qty)
                   OR (prev_qty < 0 AND qty_raw < prev_qty)
              THEN (CAST(qty_raw AS REAL) * avg_px_raw - CAST(prev_qty AS REAL) * prev_avg)
                   / (qty_raw - prev_qty)
              ELSE prev_avg + (CAST(realized_raw - prev_realized AS REAL) * 100000000.0)
                   / (prev_qty - qty_raw)
         END AS px
  FROM p
  WHERE prev_qty IS NOT NULL AND qty_raw <> prev_qty
    AND NOT EXISTS (SELECT 1 FROM fills f WHERE f.session_id = p.session_id AND f.seq = p.seq - 1)
    AND NOT EXISTS (SELECT 1 FROM funding g WHERE g.session_id = p.session_id
                    AND g.seq = p.seq - 1)),
hit AS (
  SELECT j.session_id, j.seq, j.ts_ns, j.day, f.account, j.instrument_id, j.symbol, j.side,
         f.exec_id, f.price_raw, j.moved, f.seq AS first_seq,
         ROW_NUMBER() OVER (PARTITION BY j.session_id, j.seq ORDER BY f.seq DESC) AS n
  FROM jump j
  JOIN fills f ON f.session_id = j.session_id AND f.instrument_id = j.instrument_id
              AND f.side = j.side AND f.seq < j.seq AND f.exec_id <> ''
              AND f.booked_qty_raw + f.named_qty_raw = j.moved
  JOIN instruments i ON i.session_id = j.session_id AND i.instrument_id = j.instrument_id
  WHERE i.inverse = 0 AND ABS(j.px - f.price_raw) <= i.tick_raw)
INSERT INTO fill_repeats (session_id, seq, ts_ns, day, account, instrument_id, symbol, side,
                          exec_id, price_raw, qty_raw, booked_qty_raw, first_seq)
SELECT session_id, seq - 1, ts_ns, day, account, instrument_id, symbol, side, exec_id, price_raw,
       moved, moved, first_seq
FROM hit WHERE n = 1;

DROP VIEW pnl_by_day;
CREATE VIEW pnl_by_day AS
  SELECT day, account, symbol, settlement_ccy,
         SUM(realized_raw)     AS realized_raw,
         SUM(funding_raw)      AS funding_raw,
         SUM(fees_raw)         AS fees_raw,
         SUM(realized_raw) - SUM(fees_raw) AS net_raw,
         SUM(gross_traded_raw) AS gross_traded_raw,
         SUM(fills)            AS fills
  FROM pnl_daily
  GROUP BY day, account, symbol, settlement_ccy;
)SQL";

// Version 11: the operator's risk limit changes (fastmm-ctl limits), one row per key that changed.
constexpr std::string_view kV11 = R"SQL(
CREATE TABLE limit_changes (
  session_id INTEGER NOT NULL,
  seq        INTEGER NOT NULL,
  ts_ns      INTEGER NOT NULL,
  day        TEXT    NOT NULL,
  key        TEXT    NOT NULL,
  before     TEXT    NOT NULL,
  after      TEXT    NOT NULL,
  forced     INTEGER NOT NULL,
  PRIMARY KEY (session_id, seq, key)
);
CREATE INDEX limit_changes_time ON limit_changes(ts_ns);
)SQL";

// Version 12: [ab] tests. ab_arms names each session's arms in order; a fill, an order and a
// parameter update carry the arm by name: an order the arm it was last sent under, a fill its
// order's, an update the arm it put its instruments in ('' out of the test). NULL: none.
constexpr std::string_view kV12 = R"SQL(
CREATE TABLE ab_arms (
  session_id INTEGER NOT NULL,
  arm        INTEGER NOT NULL,
  test       TEXT    NOT NULL,
  name       TEXT    NOT NULL,
  params     TEXT    NOT NULL,
  PRIMARY KEY (session_id, arm)
);
ALTER TABLE fills ADD COLUMN arm TEXT;
ALTER TABLE orders ADD COLUMN arm TEXT;
ALTER TABLE param_updates ADD COLUMN arm TEXT;
)SQL";

constexpr std::array<Migration, 12> kMigrations{Migration{1, kV1},
                                                Migration{2, kV2},
                                                Migration{3, kV3},
                                                Migration{4, kV4},
                                                Migration{5, kV5},
                                                Migration{6, kV6},
                                                Migration{7, kV7},
                                                Migration{8, kV8},
                                                Migration{9, kV9},
                                                Migration{10, kV10},
                                                Migration{11, kV11},
                                                Migration{12, kV12}};

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

Result<std::string, std::string> backup_before_migrate(sqlite3* db, const std::string& path) {
  auto cur = schema_version(db);
  if (!cur) return fail(cur.error());
  const auto steps = migrations();
  if (*cur == 0 || steps.empty() || *cur >= steps.back().version || path.empty() ||
      path == ":memory:")
    return std::string();
  std::string out = path + ".v" + std::to_string(*cur) + ".bak";
  std::error_code ec;
  if (std::filesystem::exists(out, ec)) return out;  // an earlier attempt's copy
  std::string quoted;
  for (const char c : out) {
    if (c == '\'') quoted += '\'';
    quoted += c;
  }
  const std::string sql = "VACUUM INTO '" + quoted + "'";
  if (auto r = exec(db, sql.c_str()); !r)
    return fail("cannot copy the store to " + out + " before migrating it: " + r.error());
  return out;
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

std::string_view param_type_name(std::uint8_t type) {
  switch (type) {
    case 0:
      return "int";
    case 1:
      return "double";
    case 2:
      return "bool";
    case 3:
      return "decimal";
    case 4:
      return "bps";
    case 5:
      return "ms";
    default:
      return "?";
  }
}

std::string param_text(std::int64_t raw, std::uint8_t type) {
  switch (type) {
    case 1: {
      char buf[32];
      const auto r = std::to_chars(buf, buf + sizeof buf, std::bit_cast<double>(raw));
      return {buf, r.ptr};
    }
    case 2:
      return raw != 0 ? "true" : "false";
    case 3:
      return detail::format_scaled(raw, kFixedDecimals);
    case 4:
      return detail::format_scaled(raw, kBpsDecimals);
    case 5:
      return detail::format_scaled(raw, 6);
    default:
      return std::to_string(raw);
  }
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
