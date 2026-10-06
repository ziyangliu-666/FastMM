// Query side of the SQLite backend: the rows fastmm-pnl prints and the summary a restarting
// session shows. Read-only, off any hot path.
#include "sqlite_schema.hpp"

#include "fastmm/store/sqlite_store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fastmm::store {

namespace {

using sqlite::decimal;
using sqlite::error_of;
using sqlite::utc_stamp;

// Raw fixed-point columns are rendered as exact decimals; a column whose name ends in _raw loses
// the suffix. Everything else is copied as text.
constexpr std::string_view kRawSuffix = "_raw";

bool is_raw(std::string_view name) {
  return name.size() > kRawSuffix.size() &&
         name.substr(name.size() - kRawSuffix.size()) == kRawSuffix;
}

// Columns holding a nanosecond wall clock, rendered as "YYYY-MM-DD HH:MM:SS".
bool is_ns(std::string_view name) {
  return name == "ts_ns" || name == "started_ns" || name == "stopped_ns" || name == "created_ns" ||
         name == "updated_ns" || name == "last_ns";
}

std::string cell(sqlite3_stmt* st, int i, std::string_view name) {
  if (sqlite3_column_type(st, i) == SQLITE_NULL) return {};
  if (is_raw(name)) return decimal(sqlite3_column_int64(st, i));
  if (is_ns(name)) return utc_stamp(sqlite3_column_int64(st, i));
  const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, i));
  return p == nullptr ? std::string() : std::string(p);
}

class SqliteReader final : public Reader {
 public:
  ~SqliteReader() override {
    if (db_ != nullptr) static_cast<void>(sqlite3_close_v2(db_));
  }

  [[nodiscard]] Result<void, std::string> open(const BackendOptions& opts) override {
    path_ = sqlite_path(opts);
    if (sqlite3_open_v2(path_.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
      const std::string e =
          db_ == nullptr ? "cannot open " + path_ : error_of(db_, "open " + path_);
      if (db_ != nullptr) {
        static_cast<void>(sqlite3_close_v2(db_));
        db_ = nullptr;
      }
      return fail(e);
    }
    sqlite3_busy_timeout(db_, 5000);
    auto v = sqlite::schema_version(db_);
    if (!v) return fail(v.error());
    if (*v == 0) return fail(path_ + " is not a FastMM store (no schema_version table)");
    if (*v > kSqliteSchemaVersion) {
      return fail(path_ + " has schema version " + std::to_string(*v) + "; this build knows " +
                  std::to_string(kSqliteSchemaVersion));
    }
    version_ = *v;
    return {};
  }

  // A column schema 4 added, or zero in an older store.
  [[nodiscard]] std::string v4(std::string_view column, std::string_view as = {}) const {
    const std::string name(as.empty() ? column : as);
    return version_ >= 4 ? std::string(column) + " AS " + name : "0 AS " + name;
  }

  [[nodiscard]] Result<Rows, std::string> sessions(const QueryFilter& f) override {
    std::string sql =
        "SELECT session_id, engine, strategy, started_ns, stopped_ns, dry_run, fills,"
        " realized_raw, " +
        v4("funding_raw") +
        ", unrealized_raw, fees_raw, clean_shutdown, exit_code, kill_reason,"
        " records_dropped FROM sessions";
    Where w;
    w.engine(f);
    w.session(f);
    w.days(f, "started_ns");
    return query(sql + w.text() + " ORDER BY started_ns DESC" + limit(f), w);
  }

  [[nodiscard]] Result<Rows, std::string> fills(const QueryFilter& f) override {
    std::string sql =
        "SELECT ts_ns, symbol, side, liquidity, price_raw, qty_raw, fee_raw, fee_asset,"
        " cl_ord_id, exec_id, position_qty_raw, session_id FROM fills";
    Where w;
    w.session(f);
    w.symbol(f);
    w.day_range(f);
    w.engine_join(f, "fills");
    return query(sql + w.text() + " ORDER BY ts_ns" + limit(f), w);
  }

  [[nodiscard]] Result<std::vector<AuditFill>, std::string> booked_fills(
      const BookedFillQuery& q) override {
    if (version_ < 3)
      return fail(path_ + " predates the venues' names and times (schema 3): no fill audit");
    std::string sql =
        "SELECT v.name, f.symbol, f.exec_id, f.venue_order_id, f.cl_ord_id, f.side, f.price_raw,"
        " f.qty_raw, f.fee_amount_raw, f.fee_asset, f.exch_ns, f.session_id FROM fills f"
        " JOIN session_venues v ON v.session_id = f.session_id AND v.venue_id = f.venue_id"
        " WHERE f.exec_id <> ''";
    std::vector<std::string> texts;
    std::vector<std::int64_t> ints;
    std::vector<bool> order;
    const auto text = [&](const char* clause, const std::string& v) {
      sql += clause;
      texts.push_back(v);
      order.push_back(true);
    };
    const auto num = [&](const char* clause, std::int64_t v) {
      sql += clause;
      ints.push_back(v);
      order.push_back(false);
    };
    if (!q.engine.empty())
      text(" AND f.session_id IN (SELECT session_id FROM sessions WHERE engine = ?)", q.engine);
    if (!q.venue.empty()) text(" AND v.name = ?", q.venue);
    if (!q.instrument.empty()) text(" AND f.symbol = ?", q.instrument);
    if (q.from_ms != 0) num(" AND f.exch_ns >= ?", q.from_ms * 1'000'000);
    if (q.to_ms != 0) num(" AND f.exch_ns <= ?", q.to_ms * 1'000'000 + 999'999);
    sql += " ORDER BY f.exch_ns, f.session_id, f.seq";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
      return fail(error_of(db_, "booked fills"));
    std::size_t ti = 0;
    std::size_t ii = 0;
    for (std::size_t n = 0; n < order.size(); ++n) {
      const int at = static_cast<int>(n) + 1;
      if (order[n]) {
        const std::string& v = texts[ti++];
        sqlite3_bind_text(st, at, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
      } else {
        sqlite3_bind_int64(st, at, ints[ii++]);
      }
    }
    const auto str = [st](int i) {
      const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, i));
      return p == nullptr ? std::string() : std::string(p);
    };
    std::vector<AuditFill> out;
    int rc = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      AuditFill a;
      a.venue = str(0);
      a.symbol = str(1);
      a.exec_id = str(2);
      a.order_id = str(3);
      a.cl_ord_id = str(4);
      a.side = str(5) == "Sell" ? Side::Sell : Side::Buy;
      a.price_raw = sqlite3_column_int64(st, 6);
      a.qty_raw = sqlite3_column_int64(st, 7);
      a.fee_raw = sqlite3_column_int64(st, 8);
      a.has_fee = true;
      a.fee_asset = str(9);
      a.time_ms = sqlite3_column_int64(st, 10) / 1'000'000;
      a.session_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 11));
      out.push_back(std::move(a));
    }
    const bool ok = rc == SQLITE_DONE;
    const std::string err = ok ? std::string() : error_of(db_, "booked fills");
    sqlite3_finalize(st);
    if (!ok) return fail(err);
    return out;
  }

  [[nodiscard]] Result<Rows, std::string> orders(const QueryFilter& f) override {
    std::string sql =
        "SELECT updated_ns, symbol, cl_ord_id, side, type, price_raw, qty_raw, cum_qty_raw,"
        " state, reject_reason, terminal, updates, session_id FROM orders";
    Where w;
    w.session(f);
    w.symbol(f);
    w.day_range(f);
    w.engine_join(f, "orders");
    return query(sql + w.text() + " ORDER BY updated_ns" + limit(f), w);
  }

  // Realized includes funding: `funding` says how much of it.
  [[nodiscard]] Result<Rows, std::string> pnl(const QueryFilter& f) override {
    std::string sql =
        "SELECT day, symbol, settlement_ccy, SUM(realized_raw) AS realized_raw, " +
        v4("SUM(funding_raw)", "funding_raw") +
        ", SUM(fees_raw) AS fees_raw, SUM(realized_raw) - SUM(fees_raw) AS net_raw,"
        " SUM(gross_traded_raw) AS gross_traded_raw, SUM(fills) AS fills FROM pnl_daily";
    Where w;
    w.session(f);
    w.symbol(f);
    w.day_text(f);
    w.engine_join(f, "pnl_daily");
    return query(
        sql + w.text() + " GROUP BY day, symbol, settlement_ccy ORDER BY day, symbol" + limit(f),
        w);
  }

  [[nodiscard]] Result<Rows, std::string> funding(const QueryFilter& f) override {
    if (version_ < 4) return Rows{};
    std::string sql =
        "SELECT ts_ns, symbol, amount_raw, asset, funding_id, position_qty_raw,"
        " position_funding_raw, replayed, session_id FROM funding";
    Where w;
    w.session(f);
    w.symbol(f);
    w.day_range(f);
    w.engine_join(f, "funding");
    return query(sql + w.text() + " ORDER BY ts_ns" + limit(f), w);
  }

  // An execution is the venue's id of it on one symbol and side (the two halves of a self-trade
  // share an id; two venues, and two symbols of one, can use the same number). A venue is its name
  // in the session that wrote the row (its id is its place in that session's configuration), so the
  // same execution stored by two sessions groups together whatever their venue order. Within a
  // session the unique indexes keep it once; every copy is another session's.
  //
  //
  // Two steps. The ids that occur more than once on a symbol and side come from the id indexes of
  // schema 7 alone, in index order (no row is read and nothing is sorted). Their rows - none, in
  // a sound store - are then looked up by id and grouped by engine and venue name. A store not yet
  // migrated to schema 7 has no such index: every row is grouped, as before.
  [[nodiscard]] Result<Rows, std::string> duplicates(const QueryFilter& f) override {
    const auto part = [&](std::string_view kind,
                          std::string_view table,
                          std::string_view id,
                          std::string_view side,
                          std::string_view qty) {
      const bool fill = kind == "fill";
      std::string q = "SELECT '";
      q += kind;
      q += "' AS kind, s.engine AS engine, ";
      q += version_ >= 3 ? "COALESCE(v.name, '#' || r.venue_id)" : "'#' || r.venue_id";
      q += " AS venue, r.symbol AS symbol, r.";
      q += id;
      q += " AS id, r.session_id AS session_id, s.started_ns AS started_ns, r.ts_ns AS ts_ns, ";
      q += side;
      q += " AS side, r.";
      q += qty;
      q += " AS qty_raw FROM ";
      if (version_ >= 7) {
        // The repeated ids first (CROSS JOIN keeps that order), then their rows by id.
        q += "(SELECT ";
        q += id;
        q += " AS id, symbol";
        if (fill) q += ", side";
        q += " FROM ";
        q += table;
        q += " WHERE ";
        q += id;
        q += " <> '' GROUP BY ";
        q += id;
        q += ", symbol";
        if (fill) q += ", side";
        q += " HAVING COUNT(*) > 1) c CROSS JOIN ";
        q += table;
        q += " r ON r.";
        q += id;
        q += " = c.id AND r.symbol = c.symbol";
        if (fill) q += " AND r.side = c.side";
        q += " AND r.";
        q += id;
        q += " <> ''";
      } else {
        q += table;
        q += " r";
      }
      q += " JOIN sessions s ON s.session_id = r.session_id";
      if (version_ >= 3)
        q += " LEFT JOIN session_venues v ON v.session_id = r.session_id AND"
             " v.venue_id = r.venue_id";
      if (version_ < 7) {
        q += " WHERE r.";
        q += id;
        q += " <> ''";
      }
      return q;
    };
    std::string rows = part("fill", "fills", "exec_id", "r.side", "qty_raw");
    if (version_ >= 4)
      rows += " UNION ALL " + part("funding", "funding", "funding_id", "''", "amount_raw");
    Where w;
    w.engine(f);
    w.symbol(f);
    return query(
        "SELECT kind, engine, venue, symbol, id, COUNT(*) AS copies, MIN(side) AS side,"
        " MIN(qty_raw) AS qty_raw, MIN(ts_ns) AS ts_ns, GROUP_CONCAT(session_id, ' ') AS sessions"
        " FROM (SELECT * FROM (" +
            rows + ") ORDER BY started_ns)" + w.text() +
            " GROUP BY kind, engine, venue, symbol, side, id HAVING COUNT(*) > 1"
            " ORDER BY MIN(ts_ns)" +
            limit(f),
        w);
  }

  [[nodiscard]] Result<Rows, std::string> positions(const QueryFilter& f) override {
    std::string sql =
        "SELECT p.ts_ns, p.symbol, p.qty_raw, p.avg_px_raw, p.realized_raw, " +
        v4("p.funding_raw", "funding_raw") +
        ", p.unrealized_raw, p.fees_raw, p.fills, p.session_id FROM positions p JOIN"
        " (SELECT session_id, instrument_id, MAX(seq) AS seq FROM positions GROUP BY session_id,"
        " instrument_id) l ON p.session_id = l.session_id AND p.seq = l.seq";
    Where w;
    w.session(f, "p.session_id");
    w.symbol(f, "p.symbol");
    w.engine_join(f, "p");
    return query(sql + w.text() + " ORDER BY p.session_id DESC, p.symbol" + limit(f), w);
  }

  [[nodiscard]] Result<Recovery, std::string> recovery(const QueryFilter& f) override {
    Recovery rec;
    std::string sql =
        "SELECT session_id, started_ns, stopped_ns, strategy, kill_reason, kill_latched,"
        " clean_shutdown, exit_code, realized_raw, unrealized_raw, fees_raw, fills,"
        " records_dropped, journal_complete, " +
        v4("funding_raw") + ", engine, started_ns FROM sessions";
    Where w;
    w.engine(f);
    w.session(f);
    sql += w.text();
    sql += " ORDER BY started_ns DESC LIMIT 1";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
      return fail(error_of(db_, "sessions"));
    w.bind(st);
    if (sqlite3_step(st) != SQLITE_ROW) {
      sqlite3_finalize(st);
      return rec;
    }
    rec.found = true;
    rec.session_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    rec.started_utc = utc_stamp(sqlite3_column_int64(st, 1));
    rec.stopped_utc = sqlite3_column_type(st, 2) == SQLITE_NULL
                          ? std::string()
                          : utc_stamp(sqlite3_column_int64(st, 2));
    rec.strategy = text(st, 3);
    rec.kill_reason = text(st, 4);
    rec.kill_latched = sqlite3_column_int(st, 5) != 0;
    rec.clean_shutdown = sqlite3_column_int(st, 6) != 0;
    rec.exit_code = sqlite3_column_int(st, 7);
    const std::int64_t realized = sqlite3_column_int64(st, 8);
    const std::int64_t unrealized = sqlite3_column_int64(st, 9);
    const std::int64_t fees = sqlite3_column_int64(st, 10);
    rec.realized = decimal(realized);
    rec.unrealized = decimal(unrealized);
    rec.fees = decimal(fees);
    rec.net = decimal(realized + unrealized - fees);
    rec.fills = static_cast<std::uint64_t>(sqlite3_column_int64(st, 11));
    rec.records_dropped = static_cast<std::uint64_t>(sqlite3_column_int64(st, 12));
    rec.journal_complete =
        sqlite3_column_type(st, 13) == SQLITE_NULL || sqlite3_column_int(st, 13) != 0;
    rec.funding = decimal(sqlite3_column_int64(st, 14));
    const std::string engine = text(st, 15);
    const std::int64_t started_ns = sqlite3_column_int64(st, 16);
    sqlite3_finalize(st);

    QueryFilter one;
    one.session_id = rec.session_id;
    if (auto r = positions(one); r) {
      for (const auto& row : r->rows) {
        if (row.size() < 9) continue;
        rec.positions.push_back(row[1] + " " + row[2] + " @ " + row[3] + " realized=" + row[4] +
                                " (funding " + row[5] + ") unrealized=" + row[6] +
                                " fees=" + row[7] + " fills=" + row[8]);
      }
    }
    collect(rec.open_orders,
            "SELECT cl_ord_id, symbol, side, qty_raw, cum_qty_raw, price_raw, state FROM orders"
            " WHERE session_id = ? AND terminal = 0 ORDER BY cl_ord_id",
            rec.session_id,
            [](sqlite3_stmt* s) {
              return text(s, 0) + " " + text(s, 1) + " " + text(s, 2) + " " +
                     decimal(sqlite3_column_int64(s, 3)) + " (filled " +
                     decimal(sqlite3_column_int64(s, 4)) + ") @ " +
                     decimal(sqlite3_column_int64(s, 5)) + " " + text(s, 6);
            });
    collect(rec.journals,
            "SELECT path FROM session_journals WHERE session_id = ? ORDER BY part",
            rec.session_id,
            [](sqlite3_stmt* s) { return text(s, 0); });

    const std::vector<ChainSession> chain = sessions_up_to(engine, started_ns);
    rec.position_state = last_positions(chain);
    for (const ChainSession& c : chain) {
      if (rec.session_epochs.size() == Recovery::kMaxSessionEpochs) break;
      if (c.epoch != 0 &&
          std::find(rec.session_epochs.begin(), rec.session_epochs.end(), c.epoch) ==
              rec.session_epochs.end())
        rec.session_epochs.push_back(c.epoch);
    }
    for (std::size_t k = 0; k < chain.size() && k < Recovery::kMaxSessionEpochs; ++k) {
      if (rec.past_orders.size() >= Recovery::kMaxPastOrders) break;
      const std::vector<std::string> names = venue_names(chain[k].id);
      collect(rec.past_orders,
              "SELECT venue_id, symbol, venue_order_id, cl_ord_id FROM orders WHERE session_id = ?"
              " AND terminal = 0 AND venue_order_id <> '' ORDER BY updated_ns DESC",
              chain[k].id,
              [&](sqlite3_stmt* s) {
                const std::int64_t v = sqlite3_column_int64(s, 0);
                return Recovery::PastOrder{v >= 0 && static_cast<std::size_t>(v) < names.size()
                                               ? names[static_cast<std::size_t>(v)]
                                               : std::string(),
                                           text(s, 1),
                                           text(s, 2),
                                           text(s, 3)};
              });
    }
    if (rec.past_orders.size() > Recovery::kMaxPastOrders)
      rec.past_orders.resize(Recovery::kMaxPastOrders);
    resume_points(rec, chain, engine, started_ns);
    QueryFilter mine;
    mine.engine = engine;
    if (auto dup = duplicates(mine); dup) {
      rec.duplicate_count = dup->rows.size();
      for (const std::vector<std::string>& row : dup->rows) {
        if (rec.duplicates.size() == Recovery::kMaxDuplicates) break;
        Recovery::Duplicate d;
        d.funding = row[0] == "funding";
        d.venue = row[2];
        d.symbol = row[3];
        d.id = row[4];
        d.copies = static_cast<std::uint32_t>(std::strtoul(row[5].c_str(), nullptr, 10));
        d.side = row[6];
        d.qty = row[7];
        d.sessions = row[9];
        rec.duplicates.push_back(std::move(d));
      }
    }
    return rec;
  }

 private:
  // How far a session's engine clock may be off the one before it (a host clock step).
  static constexpr std::int64_t kClockSlackNs = 60'000'000'000;

  // A stored fill: its time (venue ms, or engine ns for the fallback) and trade id.
  struct Stamped {
    std::int64_t t = 0;
    std::string id;
  };

  // The rows of `sql`, bound to the session, `from` and (when not negative) `venue`, newest first.
  // The statement asks for one row more than Recovery::kMaxKnownExecIds, so fit() sees an overflow.
  std::vector<Stamped> newest(const std::string& sql,
                              std::uint64_t session,
                              std::int64_t from,
                              int venue) {
    std::vector<Stamped> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int64(st, 1, static_cast<std::int64_t>(session));
    sqlite3_bind_int64(st, 2, from);
    if (venue >= 0) sqlite3_bind_int(st, 3, venue);
    while (sqlite3_step(st) == SQLITE_ROW)
      out.push_back(Stamped{sqlite3_column_int64(st, 0), text(st, 1)});
    sqlite3_finalize(st);
    return out;
  }

  // Keeps at most Recovery::kMaxKnownExecIds of `rows` (newest first, all at or after `since`) in
  // `ids` and returns the start they cover: `since`, or later when some did not fit. The rows that
  // share the time of the first one left out go with it, so every stored fill at or after the
  // returned start is in `ids`.
  static std::int64_t fit(std::vector<Stamped>& rows,
                          std::int64_t since,
                          std::vector<std::string>& ids) {
    if (rows.size() > Recovery::kMaxKnownExecIds) {
      const std::int64_t cut = rows[Recovery::kMaxKnownExecIds].t;
      since = cut + 1;
      while (!rows.empty() && rows.back().t <= cut) rows.pop_back();
    }
    for (Stamped& r : rows) ids.push_back(std::move(r.id));
    return since;
  }

  // One session of the engine whose state a restart carries over, newest first.
  struct ChainSession {
    std::uint64_t id = 0;
    std::int64_t started_ns = 0;
    bool clean = false;  // recorded a clean shutdown
    std::uint16_t epoch = 0;
  };

  // Every session of `engine` that started no later than `started_ns`, newest first. A session
  // that crashed before it traded, or before it stored a fill of some venue, holds nothing about
  // that venue; the state a restart needs is then in an older one.
  std::vector<ChainSession> sessions_up_to(const std::string& engine, std::int64_t started_ns) {
    std::vector<ChainSession> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT session_id, started_ns, clean_shutdown, session_epoch FROM sessions WHERE"
            " engine = ?1 AND started_ns <= ?2 ORDER BY started_ns DESC, session_id DESC",
            -1,
            &st,
            nullptr) != SQLITE_OK)
      return out;
    sqlite3_bind_text(st, 1, engine.data(), static_cast<int>(engine.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, started_ns);
    while (sqlite3_step(st) == SQLITE_ROW) {
      out.push_back(ChainSession{static_cast<std::uint64_t>(sqlite3_column_int64(st, 0)),
                                 sqlite3_column_int64(st, 1),
                                 sqlite3_column_int(st, 2) != 0,
                                 static_cast<std::uint16_t>(sqlite3_column_int(st, 3))});
    }
    sqlite3_finalize(st);
    return out;
  }

  // A venue as a restart matches it: by its name, or by its id in a session that predates names.
  static std::string venue_key(const std::string& name, std::int64_t id) {
    return name.empty() ? "#" + std::to_string(id) : name;
  }

  // The names of `session`'s venues by id (empty before schema 3).
  std::vector<std::string> venue_names(std::uint64_t session) {
    std::vector<std::string> names;
    if (version_ >= 3) {
      collect(names,
              "SELECT name FROM session_venues WHERE session_id = ? ORDER BY venue_id",
              session,
              [](sqlite3_stmt* s) { return text(s, 0); });
    }
    return names;
  }

  // Recovery::position_state: per venue and symbol, the last position the store holds, from the
  // newest session that recorded one. Within a session the last record is a position row or a
  // fill row (each fill carries the position after it): the store commits whatever the ring holds
  // when it drains, so a crash can keep a fill whose position row was still on its way, and a
  // position that missed it would be restored while the replay skips the fill as already booked.
  std::vector<Recovery::PositionState> last_positions(const std::vector<ChainSession>& chain) {
    std::vector<Recovery::PositionState> out;
    std::set<std::pair<std::string, std::string>> seen;
    for (const ChainSession& c : chain) {
      const std::vector<std::string> names = venue_names(c.id);
      std::vector<Recovery::PositionState> rows;
      collect(rows,
              "SELECT i.venue_id, i.symbol, x.qty, x.px FROM (SELECT instrument_id, seq, qty_raw AS"
              " qty, avg_px_raw AS px FROM positions WHERE session_id = ?1 UNION ALL SELECT"
              " instrument_id, seq, position_qty_raw, position_avg_px_raw FROM fills WHERE"
              " session_id = ?1) x JOIN (SELECT instrument_id, MAX(seq) AS seq FROM (SELECT"
              " instrument_id, seq FROM positions WHERE session_id = ?1 UNION ALL SELECT"
              " instrument_id, seq FROM fills WHERE session_id = ?1) GROUP BY instrument_id) l ON"
              " x.instrument_id = l.instrument_id AND x.seq = l.seq JOIN instruments i ON"
              " i.session_id = ?1 AND i.instrument_id = x.instrument_id ORDER BY x.instrument_id",
              c.id,
              [&](sqlite3_stmt* s) {
                const std::int64_t v = sqlite3_column_int64(s, 0);
                Recovery::PositionState p;
                p.venue_id = static_cast<std::uint8_t>(v);
                p.venue = v >= 0 && static_cast<std::size_t>(v) < names.size()
                              ? names[static_cast<std::size_t>(v)]
                              : std::string();
                p.symbol = text(s, 1);
                p.qty_raw = sqlite3_column_int64(s, 2);
                p.avg_px_raw = sqlite3_column_int64(s, 3);
                return p;
              });
      for (Recovery::PositionState& p : rows) {
        if (seen.insert({venue_key(p.venue, p.venue_id), p.symbol}).second)
          out.push_back(std::move(p));
      }
    }
    return out;
  }

  // Whether `session` placed an order on its venue `venue`: one that went out, not one the
  // engine's own checks refused.
  bool placed_order(std::uint64_t session, std::int64_t venue) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT 1 FROM orders WHERE session_id = ?1 AND venue_id = ?2 AND NOT"
                           " (state = 'Rejected' AND venue_order_id = '') LIMIT 1",
                           -1,
                           &st,
                           nullptr) != SQLITE_OK)
      return false;
    sqlite3_bind_int64(st, 1, static_cast<std::int64_t>(session));
    sqlite3_bind_int64(st, 2, venue);
    const bool any = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return any;
  }

  // The trade ids and funding ids (kFundingIdPrefix + id) every session of `engine` up to
  // `started_ns` stored for the venue named `venue`, at or after `since_ms` in the venue's clock:
  // newest first, each id once, one more than Recovery::kMaxKnownExecIds at most (see fit()). The
  // venue is matched by its name, session by session: its id is its place in a session's
  // configuration.
  std::vector<Stamped> stored_ids(const std::string& engine,
                                  std::int64_t started_ns,
                                  const std::string& venue,
                                  std::int64_t since_ms) {
    const auto rows_of = [](std::string_view table, std::string_view id) {
      std::string q = "SELECT r.exch_ns AS ns, ";
      q += id;
      q += " AS id FROM sessions s JOIN session_venues v ON v.session_id = s.session_id AND"
           " v.name = ?3 JOIN ";
      q += table;
      q += " r ON r.session_id = s.session_id AND r.venue_id = v.venue_id AND r.exch_ns >= ?4 *"
           " 1000000 WHERE s.engine = ?1 AND s.started_ns <= ?2";
      return q;
    };
    std::string sql = "SELECT MAX(ns) / 1000000 AS ms, id FROM (";
    sql += rows_of("fills", "r.exec_id") + " AND r.exec_id <> ''";
    if (version_ >= 4) {
      sql += " UNION ALL ";
      sql += rows_of("funding", "'" + std::string(kFundingIdPrefix) + "' || r.funding_id") +
             " AND r.funding_id <> ''";
    }
    sql += ") GROUP BY id ORDER BY ms DESC LIMIT " + std::to_string(Recovery::kMaxKnownExecIds + 1);
    std::vector<Stamped> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(st, 1, engine.data(), static_cast<int>(engine.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, started_ns);
    sqlite3_bind_text(st, 3, venue.data(), static_cast<int>(venue.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, since_ms);
    while (sqlite3_step(st) == SQLITE_ROW)
      out.push_back(Stamped{sqlite3_column_int64(st, 0), text(st, 1)});
    sqlite3_finalize(st);
    return out;
  }

  // `rows` of several sessions as one list for fit(): newest first, each id once.
  static void merge(std::vector<Stamped>& rows) {
    std::stable_sort(
        rows.begin(), rows.end(), [](const Stamped& a, const Stamped& b) { return a.t > b.t; });
    std::set<std::string> seen;
    std::erase_if(rows, [&](const Stamped& r) { return !seen.insert(r.id).second; });
    if (rows.size() > Recovery::kMaxKnownExecIds + 1) rows.resize(Recovery::kMaxKnownExecIds + 1);
  }

  // Whether `session` got past its execution replay on its venue `venue`: it recorded a
  // reconciliation that began with the venue's executions complete (schema 6, replayed_seq). A
  // session recorded before that is judged by what such a session does afterwards: it placed an
  // order there (none goes out before the venue has reconciled) or shut down cleanly.
  bool got_past_replay(const ChainSession& session, std::int64_t venue) {
    if (version_ >= 6) {
      sqlite3_stmt* st = nullptr;
      if (sqlite3_prepare_v2(db_,
                             "SELECT replayed_seq FROM session_venues WHERE session_id = ?1 AND"
                             " venue_id = ?2",
                             -1,
                             &st,
                             nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, static_cast<std::int64_t>(session.id));
        sqlite3_bind_int64(st, 2, venue);
        const bool row = sqlite3_step(st) == SQLITE_ROW;
        const bool recorded = row && sqlite3_column_type(st, 0) != SQLITE_NULL;
        const std::int64_t seq = recorded ? sqlite3_column_int64(st, 0) : 0;
        sqlite3_finalize(st);
        if (recorded) return seq > 0;
      }
    }
    return session.clean || placed_order(session.id, venue);
  }

  // Recovery::venue_resume, last_trade_ids and the engine-clock fallbacks over `chain`, the
  // sessions of `engine` up to `started_ns`.
  //
  // The ids a replay skips are the ones any of those sessions stored, not the newest one's alone:
  // a session that died seconds after it started holds only what its own replay booked, while
  // the next replay's window still reaches the executions of the sessions before it.
  //
  // Where a replay starts is where the store's record of the venue is known to be whole: up to
  // the newest session that got past its own replay (the anchor). The fills of the sessions after
  // it are known, so they are not booked again, but they do not move the start: such a session
  // died while its replay was still reading, and a fill that reached it live meanwhile is newer
  // than the ones the replay had not got to.
  void resume_points(Recovery& rec,
                     const std::vector<ChainSession>& chain,
                     const std::string& engine,
                     std::int64_t started_ns) {
    const std::string limit = " LIMIT " + std::to_string(Recovery::kMaxKnownExecIds + 1);
    constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    // A venue with no stored fill in any of these sessions: whatever it executed since the newest
    // session that shut down cleanly (its replay at connect saw everything before) is unbooked;
    // with no clean one, since the oldest session.
    std::int64_t from_ns = 0;
    for (const ChainSession& c : chain) {
      from_ns = c.started_ns;
      if (c.clean) break;
    }
    if (from_ns > Recovery::kFallbackOverlapNs)
      rec.unbooked_since_ms = (from_ns - Recovery::kFallbackOverlapNs) / 1'000'000;

    // A session's last stored fill or funding payment of one venue, venue time.
    struct Last {
      std::size_t session = 0;  // index in `chain`
      std::int64_t venue = 0;   // the venue's id in that session
      std::int64_t ms = 0;
    };
    // A session's highest numeric trade id of one symbol of a venue.
    struct Mark {
      std::size_t session = 0;
      std::int64_t venue = 0;
      std::string symbol;
      std::int64_t id = 0;
    };
    // Per venue (venue_key), newest session first; `order` keeps the venues as first met.
    std::map<std::string, std::vector<Last>> lasts;
    std::vector<std::string> order;
    std::map<std::pair<std::string, std::string>, std::vector<Mark>> marks;
    std::vector<std::pair<std::string, std::string>> mark_order;
    std::vector<std::vector<std::string>> names(chain.size());
    std::size_t fallback_session = chain.size();
    for (std::size_t k = 0; k < chain.size(); ++k) {
      const std::uint64_t session = chain[k].id;
      names[k] = venue_names(session);
      const auto name_of = [&](std::int64_t v) {
        return v >= 0 && static_cast<std::size_t>(v) < names[k].size()
                   ? names[k][static_cast<std::size_t>(v)]
                   : std::string();
      };
      if (version_ >= 3) {
        // Schema 4: funding payments are venue events too, and their ids are known like trade
        // ids.
        std::vector<Last> found;
        collect(found,
                version_ >= 4
                    ? "SELECT venue_id, MAX(ns) / 1000000 FROM (SELECT venue_id, exch_ns AS ns"
                      " FROM fills WHERE session_id = ?1 AND exch_ns > 0 UNION ALL SELECT"
                      " venue_id, exch_ns FROM funding WHERE session_id = ?1 AND exch_ns > 0)"
                      " GROUP BY venue_id ORDER BY venue_id"
                    : "SELECT venue_id, MAX(exch_ns) / 1000000 FROM fills WHERE session_id = ?"
                      " AND exch_ns > 0 GROUP BY venue_id ORDER BY venue_id",
                session,
                [k](sqlite3_stmt* s) {
                  return Last{k, sqlite3_column_int64(s, 0), sqlite3_column_int64(s, 1)};
                });
        for (const Last& l : found) {
          const std::string key = venue_key(name_of(l.venue), l.venue);
          std::vector<Last>& of = lasts[key];
          if (of.empty()) order.push_back(key);
          of.push_back(l);
        }
      }
      std::vector<Mark> found;
      collect(found,
              "SELECT venue_id, symbol, MAX(CAST(exec_id AS INTEGER)) FROM fills WHERE"
              " session_id = ? AND exec_id <> '' AND exec_id NOT GLOB '*[^0-9]*'"
              " GROUP BY venue_id, symbol ORDER BY venue_id, symbol",
              session,
              [k](sqlite3_stmt* s) {
                return Mark{k, sqlite3_column_int64(s, 0), text(s, 1), sqlite3_column_int64(s, 2)};
              });
      for (Mark& m : found) {
        std::pair<std::string, std::string> key{venue_key(name_of(m.venue), m.venue), m.symbol};
        std::vector<Mark>& of = marks[key];
        if (of.empty()) mark_order.push_back(key);
        of.push_back(std::move(m));
      }

      if (rec.last_fill_ns != 0) continue;
      std::vector<std::int64_t> last;
      collect(last,
              "SELECT COALESCE(MAX(ts_ns), 0) FROM fills WHERE session_id = ?",
              session,
              [](sqlite3_stmt* s) { return sqlite3_column_int64(s, 0); });
      rec.last_fill_ns = last.empty() ? 0 : last.front();
      if (rec.last_fill_ns > 0) fallback_session = k;
    }

    // The anchor of a venue: the newest session (its index in `chain`) that got past its replay
    // there, among the kMaxSessionEpochs newest. kNone: none of them did; `every` then says that
    // those are all the sessions there are.
    struct Anchor {
      std::size_t session = kNone;
      bool every = false;
    };
    std::map<std::string, Anchor> anchors;
    const auto anchor_of = [&](const std::string& key) -> const Anchor& {
      if (const auto it = anchors.find(key); it != anchors.end()) return it->second;
      Anchor a;
      const std::size_t n = std::min(chain.size(), Recovery::kMaxSessionEpochs);
      a.every = n == chain.size();
      for (std::size_t k = 0; k < n; ++k) {
        std::int64_t venue = -1;
        if (key.front() == '#') {
          if (names[k].empty()) venue = std::strtoll(key.c_str() + 1, nullptr, 10);
        } else if (const auto it = std::find(names[k].begin(), names[k].end(), key);
                   it != names[k].end()) {
          venue = it - names[k].begin();
        }
        if (venue >= 0 && got_past_replay(chain[k], venue)) {
          a.session = k;
          break;
        }
      }
      return anchors.emplace(key, a).first->second;
    };

    for (const std::string& key : order) {
      const std::vector<Last>& of = lasts[key];
      const Anchor& anchor = anchor_of(key);
      Recovery::VenueResume v;
      v.venue_id = static_cast<std::uint8_t>(of.front().venue);
      if (key.front() != '#') v.venue = key;
      // Whole up to the anchor: the latest fill it or a session before it stored. The sessions
      // after it hold what they hold: the earliest of their last fills, if that is earlier.
      std::int64_t whole = 0;
      std::int64_t loose = 0;
      for (const Last& l : of) {
        if (anchor.session != kNone && l.session >= anchor.session) {
          whole = std::max(whole, l.ms);
        } else {
          loose = loose == 0 ? l.ms : std::min(loose, l.ms);
        }
      }
      v.last_fill_ms = whole == 0 ? loose : loose == 0 ? whole : std::min(whole, loose);
      std::int64_t want = v.last_fill_ms - Recovery::kResumeOverlapMs;
      // No session ever got past its replay: the record is whole only up to where they began.
      if (anchor.session == kNone && anchor.every && rec.unbooked_since_ms > 0)
        want = std::min(want, rec.unbooked_since_ms);
      std::vector<Stamped> rows;
      if (!v.venue.empty()) {
        rows = stored_ids(engine, started_ns, v.venue, want);
      } else {
        // Sessions that recorded no venue names: the venue is its id in each of them.
        std::string sql =
            "SELECT ns / 1000000, id FROM (SELECT exch_ns AS ns, exec_id AS id FROM fills WHERE"
            " session_id = ?1 AND venue_id = ?3 AND exch_ns >= ?2 * 1000000 AND exec_id <> ''";
        if (version_ >= 4) {
          sql += " UNION ALL SELECT exch_ns, '" + std::string(kFundingIdPrefix) +
                 "' || funding_id FROM funding WHERE session_id = ?1 AND venue_id = ?3"
                 " AND exch_ns >= ?2 * 1000000 AND funding_id <> ''";
        }
        sql += ") ORDER BY ns DESC" + limit;
        for (const Last& l : of) {
          std::vector<Stamped> part =
              newest(sql, chain[l.session].id, want, static_cast<int>(l.venue));
          rows.insert(rows.end(),
                      std::make_move_iterator(part.begin()),
                      std::make_move_iterator(part.end()));
        }
        merge(rows);
      }
      v.since_ms = fit(rows, want, v.known_exec_ids);
      v.shrunk = v.since_ms != want;
      rec.venue_resume.push_back(std::move(v));
    }

    // Trade ids increase per symbol where they are numbers at all (Binance): the replay goes on
    // after the highest one stored up to the anchor, and the ones the sessions after it stored
    // above that are listed, to be skipped. Without an anchor there is no such id: the symbol
    // replays by time, as the venue does.
    for (const auto& key : mark_order) {
      const std::vector<Mark>& of = marks[key];
      const Anchor& anchor = anchor_of(key.first);
      if (anchor.session == kNone) continue;
      Recovery::TradeIdMark m;
      bool any = false;
      for (const Mark& x : of) {
        if (x.session < anchor.session) continue;
        if (!any) {
          m.venue_id = static_cast<std::uint8_t>(x.venue);
          any = true;
        }
        m.last_id = std::max(m.last_id, x.id);
      }
      if (!any) continue;
      if (key.first.front() != '#') m.venue = key.first;
      m.symbol = key.second;
      for (const Mark& x : of) {
        if (x.session >= anchor.session || x.id <= m.last_id) continue;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_,
                               "SELECT CAST(exec_id AS INTEGER) FROM fills WHERE session_id = ?1"
                               " AND venue_id = ?2 AND symbol = ?3 AND exec_id <> '' AND exec_id"
                               " NOT GLOB '*[^0-9]*' AND CAST(exec_id AS INTEGER) > ?4",
                               -1,
                               &st,
                               nullptr) != SQLITE_OK)
          continue;
        sqlite3_bind_int64(st, 1, static_cast<std::int64_t>(chain[x.session].id));
        sqlite3_bind_int64(st, 2, x.venue);
        sqlite3_bind_text(
            st, 3, x.symbol.data(), static_cast<int>(x.symbol.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, m.last_id);
        while (sqlite3_step(st) == SQLITE_ROW) m.known_after.push_back(sqlite3_column_int64(st, 0));
        sqlite3_finalize(st);
      }
      std::sort(m.known_after.begin(), m.known_after.end());
      m.known_after.erase(std::unique(m.known_after.begin(), m.known_after.end()),
                          m.known_after.end());
      rec.last_trade_ids.push_back(std::move(m));
    }

    if (fallback_session < chain.size()) {
      // The engine clock's ids, from every session that can hold a fill that late: one that had
      // ended (the next one had started) before the ids begin holds none.
      const std::int64_t from = rec.last_fill_ns - Recovery::kFallbackIdsNs;
      std::vector<Stamped> rows;
      for (std::size_t k = fallback_session; k < chain.size(); ++k) {
        if (k > fallback_session && chain[k - 1].started_ns < from - kClockSlackNs) break;
        std::vector<Stamped> part = newest(
            "SELECT ts_ns, exec_id FROM fills WHERE session_id = ?1 AND ts_ns >= ?2"
            " AND exec_id <> '' ORDER BY ts_ns DESC" +
                limit,
            chain[k].id,
            from,
            -1);
        rows.insert(
            rows.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
      }
      merge(rows);
      // The ids reach further back than the start; only a cut inside the start's window moves
      // it.
      const std::int64_t want = rec.last_fill_ns - Recovery::kFallbackOverlapNs;
      rec.fallback_since_ms = std::max(want, fit(rows, want, rec.fallback_exec_ids)) / 1'000'000;
    }
  }

  // Builds a WHERE clause and remembers the values to bind, in order.
  class Where {
   public:
    void engine(const QueryFilter& f) {
      if (f.engine.empty()) return;
      add("engine = ?");
      texts_.push_back(f.engine);
      order_.push_back(true);
    }
    // A table without an `engine` column joins sessions to filter on it.
    void engine_join(const QueryFilter& f, std::string_view alias) {
      if (f.engine.empty()) return;
      add(std::string(alias) + ".session_id IN (SELECT session_id FROM sessions WHERE engine = ?)");
      texts_.push_back(f.engine);
      order_.push_back(true);
    }
    void session(const QueryFilter& f, std::string_view column = "session_id") {
      if (f.session_id == 0) return;
      add(std::string(column) + " = ?");
      ints_.push_back(static_cast<std::int64_t>(f.session_id));
      order_.push_back(false);
    }
    void symbol(const QueryFilter& f, std::string_view column = "symbol") {
      if (f.instrument.empty()) return;
      add(std::string(column) + " = ?");
      texts_.push_back(f.instrument);
      order_.push_back(true);
    }
    // A `day` text column compares directly.
    void day_text(const QueryFilter& f) {
      if (!f.from.empty()) {
        add("day >= ?");
        texts_.push_back(f.from);
        order_.push_back(true);
      }
      if (!f.to.empty()) {
        add("day <= ?");
        texts_.push_back(f.to);
        order_.push_back(true);
      }
    }
    void day_range(const QueryFilter& f) { day_text(f); }
    // A nanosecond column needs the day turned into bounds.
    void days(const QueryFilter& f, std::string_view column) {
      std::int64_t first = 0;
      std::int64_t last = 0;
      if (!f.from.empty() && sqlite::day_bounds(f.from, first, last)) {
        add(std::string(column) + " >= ?");
        ints_.push_back(first);
        order_.push_back(false);
      }
      if (!f.to.empty() && sqlite::day_bounds(f.to, first, last)) {
        add(std::string(column) + " <= ?");
        ints_.push_back(last);
        order_.push_back(false);
      }
    }
    [[nodiscard]] const std::string& text() const noexcept { return where_; }
    void bind(sqlite3_stmt* st) const {
      int n = 0;
      std::size_t ti = 0;
      std::size_t ii = 0;
      for (const bool is_text : order_) {
        ++n;
        if (is_text) {
          const std::string& v = texts_[ti++];
          sqlite3_bind_text(st, n, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        } else {
          sqlite3_bind_int64(st, n, ints_[ii++]);
        }
      }
    }

   private:
    void add(const std::string& clause) {
      where_ += where_.empty() ? " WHERE " : " AND ";
      where_ += clause;
    }
    std::string where_;
    std::vector<std::string> texts_;
    std::vector<std::int64_t> ints_;
    std::vector<bool> order_;  // true: the next value comes from texts_
  };

  static std::string text(sqlite3_stmt* st, int i) {
    const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, i));
    return p == nullptr ? std::string() : std::string(p);
  }

  static std::string limit(const QueryFilter& f) {
    return f.limit == 0 ? std::string() : " LIMIT " + std::to_string(f.limit);
  }

  Result<Rows, std::string> query(const std::string& sql, const Where& w) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
      return fail(error_of(db_, sql));
    w.bind(st);
    Rows out;
    const int ncol = sqlite3_column_count(st);
    out.columns.reserve(static_cast<std::size_t>(ncol));
    for (int i = 0; i < ncol; ++i) {
      std::string name = sqlite3_column_name(st, i);
      if (is_raw(name)) name.resize(name.size() - kRawSuffix.size());
      if (name.size() > 3 && name.substr(name.size() - 3) == "_ns") name.resize(name.size() - 3);
      out.columns.push_back(std::move(name));
    }
    int rc = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      std::vector<std::string> row;
      row.reserve(static_cast<std::size_t>(ncol));
      for (int i = 0; i < ncol; ++i) row.push_back(cell(st, i, sqlite3_column_name(st, i)));
      out.rows.push_back(std::move(row));
    }
    const bool ok = rc == SQLITE_DONE;
    const std::string err = ok ? std::string() : error_of(db_, "query");
    sqlite3_finalize(st);
    if (!ok) return fail(err);
    return out;
  }

  template <class T, class F>
  void collect(std::vector<T>& out, const char* sql, std::uint64_t session, F&& fmt) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, static_cast<std::int64_t>(session));
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(fmt(st));
    sqlite3_finalize(st);
  }
  sqlite3* db_ = nullptr;
  std::string path_;
  int version_ = 0;
};

}  // namespace

std::unique_ptr<Reader> make_sqlite_reader() {
  return std::make_unique<SqliteReader>();
}

}  // namespace fastmm::store
