// Query side of the SQLite backend: the rows fastmm-pnl prints and the summary a restarting
// session shows. Read-only, off any hot path.
#include "sqlite_schema.hpp"

#include "fastmm/store/sqlite_store.hpp"

#include <sqlite3.h>

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

  [[nodiscard]] Result<Rows, std::string> sessions(const QueryFilter& f) override {
    std::string sql =
        "SELECT session_id, engine, strategy, started_ns, stopped_ns, dry_run, fills,"
        " realized_raw, unrealized_raw, fees_raw, clean_shutdown, exit_code, kill_reason,"
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

  [[nodiscard]] Result<Rows, std::string> pnl(const QueryFilter& f) override {
    std::string sql =
        "SELECT day, symbol, settlement_ccy, SUM(realized_raw) AS realized_raw,"
        " SUM(fees_raw) AS fees_raw, SUM(realized_raw) - SUM(fees_raw) AS net_raw,"
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

  [[nodiscard]] Result<Rows, std::string> positions(const QueryFilter& f) override {
    std::string sql =
        "SELECT p.ts_ns, p.symbol, p.qty_raw, p.avg_px_raw, p.realized_raw, p.unrealized_raw,"
        " p.fees_raw, p.fills, p.session_id FROM positions p JOIN"
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
        " records_dropped, journal_complete FROM sessions";
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
    sqlite3_finalize(st);

    QueryFilter one;
    one.session_id = rec.session_id;
    if (auto r = positions(one); r) {
      for (const auto& row : r->rows) {
        if (row.size() < 8) continue;
        rec.positions.push_back(row[1] + " " + row[2] + " @ " + row[3] + " realized=" + row[4] +
                                " unrealized=" + row[5] + " fees=" + row[6] + " fills=" + row[7]);
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
    return rec;
  }

 private:
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

  template <class F>
  void collect(std::vector<std::string>& out, const char* sql, std::uint64_t session, F&& fmt) {
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
