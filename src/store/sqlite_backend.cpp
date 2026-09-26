// The SQLite storage backend: records in, rows out. See docs/reference/storage.md.
#include "sqlite_schema.hpp"

#include "fastmm/core/log.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/store/sqlite_store.hpp"

#include <sqlite3.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fastmm::store {

namespace {

using sqlite::decimal;
using sqlite::error_of;
using sqlite::exec;
using sqlite::utc_day;

// RAII for a prepared statement.
class Stmt {
 public:
  Stmt() = default;
  ~Stmt() { reset(); }
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  [[nodiscard]] Result<void, std::string> prepare(sqlite3* db, const char* sql) {
    reset();
    if (sqlite3_prepare_v2(db, sql, -1, &st_, nullptr) != SQLITE_OK)
      return fail(error_of(db, std::string("prepare: ") + sql));
    return {};
  }
  void reset() noexcept {
    if (st_ != nullptr) sqlite3_finalize(st_);
    st_ = nullptr;
  }
  [[nodiscard]] sqlite3_stmt* get() const noexcept { return st_; }
  explicit operator bool() const noexcept { return st_ != nullptr; }

 private:
  sqlite3_stmt* st_ = nullptr;
};

// Binds by position, starting at 1; each call advances.
class Bind {
 public:
  explicit Bind(sqlite3_stmt* st) noexcept : st_(st) { sqlite3_reset(st); }
  void i(std::int64_t v) noexcept { sqlite3_bind_int64(st_, ++n_, v); }
  void u(std::uint64_t v) noexcept { sqlite3_bind_int64(st_, ++n_, static_cast<std::int64_t>(v)); }
  // A default-constructed view has a null pointer, which would bind NULL into a NOT NULL column.
  void t(std::string_view v) noexcept {
    sqlite3_bind_text(st_,
                      ++n_,
                      v.data() == nullptr ? "" : v.data(),
                      static_cast<int>(v.size()),
                      SQLITE_TRANSIENT);
  }
  void b(bool v) noexcept { sqlite3_bind_int64(st_, ++n_, v ? 1 : 0); }

 private:
  sqlite3_stmt* st_;
  int n_ = 0;
};

std::string to_str(const FixedString<40>& s) {
  return std::string(s.view());
}

std::string fee_asset_name(FeeAsset a) {
  switch (a) {
    case FeeAsset::Quote:
      return "quote";
    case FeeAsset::Base:
      return "base";
    case FeeAsset::Other:
      return "other";
  }
  return "?";
}

// ---- the backend ------------------------------------------------------------------------------

class SqliteBackend final : public Backend {
 public:
  ~SqliteBackend() override { close(); }

  [[nodiscard]] std::string_view name() const noexcept override { return "sqlite"; }

  [[nodiscard]] Result<void, std::string> open(const BackendOptions& opts) override {
    const std::string path = sqlite_path(opts);
    if (const std::filesystem::path p(path); p.has_parent_path()) {
      std::error_code ec;
      std::filesystem::create_directories(p.parent_path(), ec);
    }
    const int flags =
        opts.read_only ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
      const std::string e = db_ == nullptr ? "cannot open " + path : error_of(db_, "open " + path);
      close();
      return fail(e);
    }
    path_ = path;
    sqlite3_busy_timeout(db_, 5000);
    if (!opts.read_only) {
      // WAL: a reader (fastmm-pnl, a notebook) never blocks the writer, and a crash recovers from
      // the log rather than losing the file. synchronous=NORMAL fsyncs the WAL at a checkpoint,
      // not per commit: a committed batch survives the process dying, and power loss can cost the
      // last checkpoint interval. docs/reference/storage.md explains the trade.
      if (auto r = exec(db_,
                        "PRAGMA journal_mode=WAL;"
                        "PRAGMA synchronous=NORMAL;"
                        "PRAGMA foreign_keys=ON;"
                        "PRAGMA wal_autocheckpoint=256");
          !r) {
        return fail(r.error());
      }
      if (auto r = sqlite::migrate(db_); !r) return fail(r.error());
      if (auto r = prepare_all(); !r) return fail(r.error());
    } else if (auto r = sqlite::schema_version(db_); !r) {
      return fail(r.error());
    }
    return {};
  }

  [[nodiscard]] Result<void, std::string> session_open(const SessionOpen& s) override {
    char hash[32];
    std::snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(s.config_hash));
    Bind b(ins_session_.get());
    b.u(s.session_id);
    b.t(s.engine_name);
    b.t(s.strategy);
    b.i(s.session_epoch);
    b.i(s.started_ns);
    b.t(utc_day(s.started_ns));
    b.t(s.version);
    b.t(s.build_info);
    b.t(hash);
    b.t(s.config_toml);
    b.t(s.host);
    b.i(s.pid);
    b.b(s.dry_run);
    b.i(s.pnl_carry_raw);
    if (sqlite3_step(ins_session_.get()) != SQLITE_DONE)
      return fail(error_of(db_, "insert session"));
    if (!s.journal_path.empty()) {
      Bind j(ins_journal_.get());
      j.u(s.session_id);
      j.i(0);
      j.t(s.journal_path);
      if (sqlite3_step(ins_journal_.get()) != SQLITE_DONE)
        return fail(error_of(db_, "insert journal"));
    }
    for (std::size_t v = 0; v < s.venues.size(); ++v) {
      Bind n(ins_venue_.get());
      n.u(s.session_id);
      n.i(static_cast<std::int64_t>(v));
      n.t(s.venues[v]);
      if (sqlite3_step(ins_venue_.get()) != SQLITE_DONE) return fail(error_of(db_, "insert venue"));
    }
    return {};
  }

  [[nodiscard]] Result<void, std::string> instruments(std::uint64_t session_id,
                                                      std::span<const Instrument> table) override {
    symbols_.assign(kMaxInstruments, std::string());
    ccy_.assign(kMaxInstruments, std::string());
    prev_.assign(kMaxInstruments, Prev{});
    if (auto r = exec(db_, "BEGIN"); !r) return fail(r.error());
    for (const Instrument& i : table) {
      if (i.id.value < symbols_.size()) {
        symbols_[i.id.value] = std::string(i.symbol.view());
        ccy_[i.id.value] = std::string(i.settlement_ccy());
      }
      Bind b(ins_instrument_.get());
      b.u(session_id);
      b.i(i.id.value);
      b.i(i.venue.value);
      b.t(i.symbol.view());
      b.t(i.base.view());
      b.t(i.quote.view());
      b.t(i.settlement_ccy());
      b.t(to_string(i.asset_class));
      b.b(i.inverse());
      b.i(i.tick.raw);
      b.i(i.lot.raw);
      b.i(i.contract_multiplier.raw);
      if (sqlite3_step(ins_instrument_.get()) != SQLITE_DONE) {
        const std::string e = error_of(db_, "insert instrument");
        static_cast<void>(exec(db_, "ROLLBACK"));
        return fail(e);
      }
    }
    if (auto r = exec(db_, "COMMIT"); !r) return fail(r.error());
    return {};
  }

  void begin() override {
    if (db_ == nullptr || in_batch_) return;
    if (auto r = exec(db_, "BEGIN"); !r) {
      note(r.error());
      return;
    }
    in_batch_ = true;
  }

  void commit() override {
    if (!in_batch_) return;
    in_batch_ = false;
    if (auto r = exec(db_, "COMMIT"); !r) {
      note(r.error());
      static_cast<void>(exec(db_, "ROLLBACK"));
    }
  }

  void fill(const FillRecord& r) override {
    if (db_ == nullptr) return;
    const std::uint32_t id = r.hdr.instrument.value;
    Bind b(ins_fill_.get());
    b.u(r.hdr.session_id);
    b.u(r.hdr.seq);
    b.i(r.hdr.engine_ts.ns);
    b.t(utc_day(r.hdr.engine_ts.ns));
    b.i(id);
    b.i(r.hdr.venue.value);
    b.t(symbol(id));
    b.t(encode_cl_ord_id(r.cl_ord_id).view());
    b.t(to_str(r.venue_order_id));
    b.t(to_str(r.exec_id));
    b.t(to_string(r.side));
    b.t(to_string(r.liquidity));
    b.i(r.price.raw);
    b.i(r.qty.raw);
    b.i(r.booked_qty.raw);
    b.i(r.cum_qty.raw);
    b.i(r.leaves_qty.raw);
    b.i(r.fee.raw);
    b.i(r.fee_amount.raw);
    b.t(fee_asset_name(r.fee_asset));
    b.i(r.position_qty.raw);
    b.i(r.position_avg_px.raw);
    b.i(r.position_realized.raw);
    b.i(r.position_fees.raw);
    b.b((r.hdr.flags & RecordHeader::kSynthetic) != 0);
    b.b((r.hdr.flags & RecordHeader::kLate) != 0);
    b.i(r.hdr.exch_ts.ns);
    step(ins_fill_.get(), "insert fill");
  }

  void order(const OrderRecord& r) override {
    if (db_ == nullptr) return;
    const Order& o = r.order;
    // aux[2]: the id a cancel-replace superseded. Its row keeps the price and quantity it had;
    // only its state changes, because the order carries on under another id.
    if (r.hdr.aux[2] != 0) {
      Bind b(upd_replaced_.get());
      b.i(r.hdr.engine_ts.ns);
      b.u(r.hdr.session_id);
      b.t(encode_cl_ord_id(o.cl_ord_id).view());
      step(upd_replaced_.get(), "close a replaced order");
      return;
    }
    Bind b(ins_order_.get());
    b.u(r.hdr.session_id);
    b.t(encode_cl_ord_id(o.cl_ord_id).view());
    b.i(o.instrument.value);
    b.i(o.venue.value);
    b.t(symbol(o.instrument.value));
    b.t(to_str(o.venue_order_id));
    b.t(to_string(o.side));
    b.t(to_string(o.type));
    b.t(to_string(o.tif));
    b.i(o.price.raw);
    b.i(o.qty.raw);
    b.i(o.cum_qty.raw);
    b.t(to_string(o.state));
    b.t(o.state == OrderState::Rejected ? to_string(o.reject_reason) : std::string_view());
    b.b(is_terminal(o.state));
    b.i(o.user_tag);
    b.i(o.flags);
    b.i(o.created.ns);
    b.i(r.hdr.engine_ts.ns);
    b.t(utc_day(r.hdr.engine_ts.ns));
    step(ins_order_.get(), "upsert order");
  }

  void position(const PositionRecord& r) override {
    if (db_ == nullptr) return;
    const std::uint32_t id = r.hdr.instrument.value;
    const Position& p = r.pos;
    Bind b(ins_position_.get());
    b.u(r.hdr.session_id);
    b.u(r.hdr.seq);
    b.i(r.hdr.engine_ts.ns);
    b.t(utc_day(r.hdr.engine_ts.ns));
    b.i(id);
    b.t(symbol(id));
    b.i(p.qty.raw);
    b.i(p.avg_px.raw);
    b.i(p.realized.raw);
    b.i(p.unrealized.raw);
    b.i(p.fees.raw);
    b.i(p.gross_traded.raw);
    b.i(p.fills);
    b.i(r.total_realized.raw);
    b.i(r.total_unrealized.raw);
    b.i(r.total_fees.raw);
    b.i(r.pnl_carry.raw);
    step(ins_position_.get(), "insert position");
    roll_up(r);
  }

  void kill(const KillRecord& r) override {
    if (db_ == nullptr) return;
    const bool per_venue = (r.hdr.flags & RecordHeader::kVenue) != 0;
    Bind b(ins_kill_.get());
    b.u(r.hdr.session_id);
    b.u(r.hdr.seq);
    b.i(r.hdr.engine_ts.ns);
    b.t(utc_day(r.hdr.engine_ts.ns));
    b.t(per_venue ? "venue" : "global");
    b.i(per_venue ? r.hdr.venue.value : -1);
    b.t(to_string(r.reason));
    b.i(r.kill_flags);
    b.i(r.realized.raw);
    b.i(r.unrealized.raw);
    b.i(r.fees.raw);
    b.i(r.pnl_carry.raw);
    step(ins_kill_.get(), "insert kill event");
  }

  [[nodiscard]] Result<void, std::string> session_close(const SessionClose& s) override {
    if (db_ == nullptr) return {};
    commit();
    Bind b(upd_session_.get());
    b.i(s.stopped_ns);
    b.b(true);
    b.i(s.exit_code);
    b.t(to_string(s.kill_reason));
    b.b(s.kill_latched);
    b.b(s.journal_complete);
    b.u(s.journal_bytes);
    b.u(s.stats.events);
    b.u(s.stats.orders_sent);
    b.u(s.stats.cancels_sent);
    b.u(s.stats.replaces_sent);
    b.u(s.stats.fills);
    b.u(s.stats.risk_rejects);
    b.u(s.stats.venue_rejects);
    b.u(s.stats.records_dropped);
    b.i(s.stats.realized_pnl_raw);
    b.i(s.stats.unrealized_pnl_raw);
    b.i(s.stats.fees_raw);
    b.u(s.session_id);
    if (sqlite3_step(upd_session_.get()) != SQLITE_DONE)
      return fail(error_of(db_, "update session"));
    for (std::size_t i = 0; i < s.journal_paths.size(); ++i) {
      Bind j(ins_journal_.get());
      j.u(s.session_id);
      j.i(static_cast<std::int64_t>(i));
      j.t(s.journal_paths[i]);
      static_cast<void>(sqlite3_step(ins_journal_.get()));
    }
    return {};
  }

  void close() override {
    commit();
    ins_session_.reset();
    upd_session_.reset();
    ins_journal_.reset();
    ins_instrument_.reset();
    ins_fill_.reset();
    ins_order_.reset();
    upd_replaced_.reset();
    ins_position_.reset();
    ins_kill_.reset();
    ins_pnl_.reset();
    if (db_ != nullptr) {
      static_cast<void>(sqlite3_close_v2(db_));
      db_ = nullptr;
    }
  }

  [[nodiscard]] std::uint64_t rows() const noexcept override { return rows_; }
  [[nodiscard]] std::uint64_t errors() const noexcept override { return errors_; }
  [[nodiscard]] std::string last_error() const override { return last_error_; }

 private:
  // The cumulative counters of the previous snapshot of an instrument, so a day gets the change
  // rather than the running total. Within a session a PositionTracker starts at zero.
  struct Prev {
    std::int64_t realized = 0;
    std::int64_t fees = 0;
    std::int64_t gross = 0;
    std::int64_t fills = 0;
  };

  [[nodiscard]] std::string_view symbol(std::uint32_t id) const noexcept {
    return id < symbols_.size() ? std::string_view(symbols_[id]) : std::string_view();
  }
  [[nodiscard]] std::string_view ccy(std::uint32_t id) const noexcept {
    return id < ccy_.size() ? std::string_view(ccy_[id]) : std::string_view();
  }

  void note(std::string e) {
    ++errors_;
    if (errors_ == 1) FASTMM_LOG_ERROR("store: {}", std::string_view(e));
    last_error_ = std::move(e);
  }

  void step(sqlite3_stmt* st, const char* what) {
    const int rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) {
      ++rows_;
      return;
    }
    if (rc == SQLITE_CONSTRAINT) return;  // the same record again: nothing to do
    note(error_of(db_, what));
  }

  // Adds this snapshot's change to the day's roll-up. A change that straddles midnight lands on
  // the day of the snapshot that reports it.
  void roll_up(const PositionRecord& r) {
    const std::uint32_t id = r.hdr.instrument.value;
    if (id >= prev_.size()) return;
    Prev& prev = prev_[id];
    const Position& p = r.pos;
    const std::int64_t d_realized = p.realized.raw - prev.realized;
    const std::int64_t d_fees = p.fees.raw - prev.fees;
    const std::int64_t d_gross = p.gross_traded.raw - prev.gross;
    const std::int64_t d_fills = static_cast<std::int64_t>(p.fills) - prev.fills;
    prev = Prev{p.realized.raw, p.fees.raw, p.gross_traded.raw, static_cast<std::int64_t>(p.fills)};
    Bind b(ins_pnl_.get());
    b.u(r.hdr.session_id);
    b.t(utc_day(r.hdr.engine_ts.ns));
    b.i(id);
    b.t(symbol(id));
    b.t(ccy(id));
    b.i(d_realized);
    b.i(d_fees);
    b.i(p.unrealized.raw);
    b.i(p.qty.raw);
    b.i(d_gross);
    b.i(d_fills);
    b.i(r.hdr.engine_ts.ns);
    step(ins_pnl_.get(), "upsert pnl_daily");
  }

  [[nodiscard]] Result<void, std::string> prepare_all() {
    struct Prep {
      Stmt* stmt;
      const char* sql;
    };
    const Prep preps[] = {
        {&ins_session_,
         "INSERT OR REPLACE INTO sessions (session_id, engine, strategy, session_epoch, started_ns,"
         " started_day, version, build, config_hash, config_toml, host, pid, dry_run,"
         " pnl_carry_raw) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)"},
        {&upd_session_,
         "UPDATE sessions SET stopped_ns=?, clean_shutdown=?, exit_code=?, kill_reason=?,"
         " kill_latched=?, journal_complete=?, journal_bytes=?, events=?, orders_sent=?,"
         " cancels_sent=?, replaces_sent=?, fills=?, risk_rejects=?, venue_rejects=?,"
         " records_dropped=?, realized_raw=?, unrealized_raw=?, fees_raw=? WHERE session_id=?"},
        {&ins_journal_, "INSERT OR REPLACE INTO session_journals VALUES (?,?,?)"},
        {&ins_instrument_, "INSERT OR REPLACE INTO instruments VALUES (?,?,?,?,?,?,?,?,?,?,?,?)"},
        {&ins_fill_,
         "INSERT OR IGNORE INTO fills VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,"
         "?,?,?,?,?,?,?)"},
        {&ins_venue_, "INSERT OR REPLACE INTO session_venues VALUES (?,?,?)"},
        {&ins_order_,
         "INSERT INTO orders (session_id, cl_ord_id, instrument_id, venue_id, symbol,"
         " venue_order_id, side, type, tif, price_raw, qty_raw, cum_qty_raw, state, reject_reason,"
         " terminal, user_tag, flags, created_ns, updated_ns, day, updates)"
         " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,1)"
         " ON CONFLICT(session_id, cl_ord_id) DO UPDATE SET"
         " venue_order_id=excluded.venue_order_id, price_raw=excluded.price_raw,"
         " qty_raw=excluded.qty_raw, cum_qty_raw=excluded.cum_qty_raw, state=excluded.state,"
         " reject_reason=excluded.reject_reason, terminal=excluded.terminal,"
         " updated_ns=excluded.updated_ns, updates=orders.updates+1"},
        {&upd_replaced_,
         "UPDATE orders SET state='Replaced', terminal=1, updated_ns=?,"
         " updates=orders.updates+1 WHERE session_id=? AND cl_ord_id=?"},
        {&ins_position_,
         "INSERT OR IGNORE INTO positions VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,"
         "?)"},
        {&ins_kill_, "INSERT OR IGNORE INTO kill_events VALUES (?,?,?,?,?,?,?,?,?,?,?,?)"},
        {&ins_pnl_,
         "INSERT INTO pnl_daily (session_id, day, instrument_id, symbol, settlement_ccy,"
         " realized_raw, fees_raw, unrealized_raw, qty_raw, gross_traded_raw, fills, last_ns)"
         " VALUES (?,?,?,?,?,?,?,?,?,?,?,?)"
         " ON CONFLICT(session_id, day, instrument_id) DO UPDATE SET"
         " realized_raw=pnl_daily.realized_raw+excluded.realized_raw,"
         " fees_raw=pnl_daily.fees_raw+excluded.fees_raw,"
         " gross_traded_raw=pnl_daily.gross_traded_raw+excluded.gross_traded_raw,"
         " fills=pnl_daily.fills+excluded.fills,"
         " unrealized_raw=excluded.unrealized_raw, qty_raw=excluded.qty_raw,"
         " last_ns=excluded.last_ns"},
    };
    for (const Prep& p : preps) {
      if (auto r = p.stmt->prepare(db_, p.sql); !r) return fail(r.error());
    }
    return {};
  }

  sqlite3* db_ = nullptr;
  std::string path_;
  bool in_batch_ = false;
  std::uint64_t rows_ = 0;
  std::uint64_t errors_ = 0;
  std::string last_error_;
  std::vector<std::string> symbols_;
  std::vector<std::string> ccy_;
  std::vector<Prev> prev_;
  Stmt ins_session_;
  Stmt upd_session_;
  Stmt ins_journal_;
  Stmt ins_instrument_;
  Stmt ins_fill_;
  Stmt ins_venue_;
  Stmt ins_order_;
  Stmt upd_replaced_;
  Stmt ins_position_;
  Stmt ins_kill_;
  Stmt ins_pnl_;
};

}  // namespace

std::string sqlite_path(const BackendOptions& opts) {
  if (opts.config != nullptr) {
    const std::string p = opts.config->get_string("path");
    if (!p.empty()) return p;
  }
  const std::string dir = opts.default_dir.empty() ? std::string(".") : opts.default_dir;
  const std::string name = opts.engine_name.empty() ? std::string("fastmm") : opts.engine_name;
  return dir + "/" + name + ".db";
}

std::unique_ptr<Backend> make_sqlite_backend() {
  return std::make_unique<SqliteBackend>();
}

}  // namespace fastmm::store
