// Fill decision records in the SQLite store (schema 16): each fill's order trail and the strategy's
// quote context, the names of the context values, fastmm-pnl fill-context, and a store from before
// them migrated with its old fills left without one.
#include "../../src/store/sqlite_schema.hpp"
#include "store_test_util.hpp"

#include "fastmm/store/sqlite_store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

using namespace fastmm;
using namespace fastmm::store;
using fastmm::test::kDay1Ns;
using fastmm::test::tmp_dir;

namespace {

std::string fresh(const char* name) {
  const auto p = tmp_dir() / name;
  std::error_code ec;
  for (const char* suffix : {"", "-wal", "-shm", ".v15.bak"})
    std::filesystem::remove(p.string() + suffix, ec);
  return p.string();
}

std::string text(const std::string& path, const char* sql) {
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
  sqlite3_stmt* st = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
  std::string v = "<none>";
  if (sqlite3_step(st) == SQLITE_ROW) {
    const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    v = p == nullptr ? "<null>" : p;
  }
  sqlite3_finalize(st);
  sqlite3_close_v2(db);
  return v;
}

std::unique_ptr<Backend> open_backend(const std::string& path, GenericSection& section) {
  section.values["path"] = path;
  BackendOptions o;
  o.config = &section;
  o.engine_name = "test";
  auto b = make_sqlite_backend();
  REQUIRE(b->open(o));
  return b;
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

ContextNamesRecord names(std::uint64_t session, std::uint64_t seq) {
  ContextNamesRecord r{};
  r.hdr.len = sizeof r;
  r.hdr.type = RecordType::ContextNames;
  r.hdr.session_id = session;
  r.hdr.seq = seq;
  r.hdr.engine_ts = Timestamp{kDay1Ns};
  r.names[0] = "fair";
  r.names[1] = "distance_bps";
  return r;
}

std::size_t column(const Rows& r, const std::string& name) {
  const auto it = std::find(r.columns.begin(), r.columns.end(), name);
  REQUIRE_MESSAGE(it != r.columns.end(), "no column " << name);
  return static_cast<std::size_t>(it - r.columns.begin());
}

}  // namespace

TEST_CASE("store.fill_context: each fill keeps its order's trail and the strategy's context") {
  const std::string path = fresh("fill_context.db");
  const InstrumentTable table = fastmm::test::store_table();
  constexpr std::int64_t kMs = 1'000'000;
  {
    GenericSection section;
    auto b = open_backend(path, section);
    REQUIRE(b->session_open(fastmm::test::store_session(1, kDay1Ns)));
    REQUIRE(b->instruments(1, std::span<const Instrument>(table.data(), table.size())));
    b->begin();
    b->context_names(names(1, 1));
    // Placed at t, moved once at t+20 ms, a cancel in flight since t+95 ms, filled at t+100 ms.
    FillRecord f = fastmm::test::store_fill(
        1, 2, kDay1Ns + 100 * kMs, Side::Buy, 10'000'000'000, 100'000, 0, 0, "E1");
    f.trail = 1;
    f.order_state = OrderState::PendingCancel;
    f.placed_ts = Timestamp{kDay1Ns};
    f.placed_px = Price::from_raw(9'990'000'000);
    f.sent_ts = Timestamp{kDay1Ns + 20 * kMs};
    f.venue_ack_ts = Timestamp{kDay1Ns + 22 * kMs};
    f.local_ack_ts = Timestamp{kDay1Ns + 23 * kMs};
    f.amends = 1;
    f.pending_ts = Timestamp{kDay1Ns + 95 * kMs};
    f.order_context.v[0] = 100.05;
    f.order_context.v[1] = 5.0;
    f.fill_context.v[0] = 99.8;
    f.fill_context.v[1] = 5.0;
    b->fill(f);
    // A replace in flight.
    FillRecord g = fastmm::test::store_fill(
        1, 3, kDay1Ns + 200 * kMs, Side::Sell, 10'010'000'000, 100'000, 0, 0, "E2");
    g.trail = 1;
    g.order_state = OrderState::PendingReplace;
    g.placed_ts = Timestamp{kDay1Ns + 150 * kMs};
    g.sent_ts = g.placed_ts;
    g.placed_px = g.price;
    g.pending_ts = Timestamp{kDay1Ns + 199 * kMs};
    g.pending_px = Price::from_raw(10'020'000'000);
    g.order_context.v[0] = 100.0;
    b->fill(g);
    // An execution of an order the engine never had: no trail, the side's context only.
    FillRecord u = fastmm::test::store_fill(
        1, 4, kDay1Ns + 300 * kMs, Side::Sell, 10'010'000'000, 100'000, 0, 0, "E3");
    u.fill_context.v[0] = 100.1;
    b->fill(u);
    b->commit();
    CHECK(b->errors() == 0);
    CHECK(b->last_error().empty());
  }
  CHECK(text(path, "SELECT group_concat(name, ',') FROM quote_context_names WHERE name <> ''") ==
        "fair,distance_bps");
  CHECK(text(path, "SELECT order_state FROM fills WHERE exec_id = 'E1'") == "PendingCancel");
  CHECK(text(path, "SELECT in_flight FROM fills WHERE exec_id = 'E1'") == "cancel");
  CHECK(text(path, "SELECT in_flight FROM fills WHERE exec_id = 'E2'") == "replace");
  CHECK(text(path, "SELECT in_flight_px_raw FROM fills WHERE exec_id = 'E2'") == "10020000000");
  CHECK(text(path, "SELECT amends FROM fills WHERE exec_id = 'E1'") == "1");
  CHECK(text(path, "SELECT order_ctx0 FROM fills WHERE exec_id = 'E1'") == "100.05");
  CHECK(text(path, "SELECT order_ctx2 FROM fills WHERE exec_id = 'E1'") == "<null>");
  CHECK(text(path, "SELECT order_state FROM fills WHERE exec_id = 'E3'") == "<null>");
  CHECK(text(path, "SELECT placed_ns FROM fills WHERE exec_id = 'E3'") == "<null>");
  CHECK(text(path, "SELECT fill_ctx0 FROM fills WHERE exec_id = 'E3'") == "100.1");

  GenericSection section;
  auto reader = open_reader(path, section);
  QueryFilter all;
  const auto rows = reader->fill_context(all);
  REQUIRE(rows);
  REQUIRE(rows->rows.size() == 3);
  // The context columns carry the strategy's names; slots nobody filled are left out.
  CHECK(std::find(rows->columns.begin(), rows->columns.end(), "sent_ctx2") == rows->columns.end());
  const auto& r0 = rows->rows[0];
  CHECK(r0[column(*rows, "state")] == "PendingCancel");
  CHECK(r0[column(*rows, "placed_px")] == "99.9");
  CHECK(r0[column(*rows, "amends")] == "1");
  CHECK(r0[column(*rows, "rested_ms")] == "80.0");
  CHECK(r0[column(*rows, "in_flight")] == "cancel");
  CHECK(r0[column(*rows, "in_flight_ms")] == "5.0");
  CHECK(r0[column(*rows, "sent_fair")] == "100.05");
  CHECK(r0[column(*rows, "fill_fair")] == "99.8");
  CHECK(r0[column(*rows, "sent_distance_bps")] == "5.0");
  CHECK(rows->rows[1][column(*rows, "in_flight_px")] == "100.2");
  CHECK(rows->rows[2][column(*rows, "state")].empty());
  CHECK(rows->rows[2][column(*rows, "fill_fair")] == "100.1");

  QueryFilter one;
  one.order = encode_cl_ord_id(make_cl_ord_id(7, 3)).view();
  const auto order = reader->fill_context(one);
  REQUIRE(order);
  REQUIRE(order->rows.size() == 1);
  CHECK(order->rows[0][column(*order, "in_flight")] == "replace");
}

TEST_CASE("store.fill_context: a store at schema 15 is copied, migrated, its fills without one") {
  const std::string path = fresh("fill_context_migrate.db");
  {
    sqlite3* db = nullptr;
    REQUIRE(
        sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) ==
        SQLITE_OK);
    REQUIRE(sqlite::migrate(db, 15));
    REQUIRE(
        sqlite::exec(db,
                     "INSERT INTO fills (session_id, seq, ts_ns, day, instrument_id, venue_id,"
                     " symbol, cl_ord_id, venue_order_id, exec_id, side, liquidity, price_raw,"
                     " qty_raw, booked_qty_raw, cum_qty_raw, leaves_qty_raw, fee_raw,"
                     " fee_amount_raw, fee_asset, position_qty_raw, position_avg_px_raw,"
                     " position_realized_raw, position_fees_raw, synthetic, late) VALUES"
                     " (1, 1, 1709510400000000000, '2024-03-04', 0, 0, 'BTCUSDT', 'x', 'V1',"
                     " 'OLD', 'Buy', 'Maker', 1, 1, 1, 1, 0, 0, 0, 'quote', 1, 1, 0, 0, 0, 0)"));
    sqlite3_close_v2(db);
  }
  {
    GenericSection section;
    auto b = open_backend(path, section);
    b->close();
  }
  REQUIRE(std::filesystem::exists(path + ".v15.bak"));
  CHECK(text(path + ".v15.bak", "SELECT version FROM schema_version") == "15");
  CHECK(text(path, "SELECT version FROM schema_version") == "16");
  CHECK(text(path, "SELECT count(*) FROM fills") == "1");
  CHECK(text(path,
             "SELECT COALESCE(order_state, placed_ns, order_ctx0, fill_ctx0, 'none') FROM"
             " fills") == "none");
  GenericSection section;
  auto reader = open_reader(path, section);
  const auto rows = reader->fill_context(QueryFilter{});
  REQUIRE(rows);
  REQUIRE(rows->rows.size() == 1);
  CHECK(rows->rows[0][column(*rows, "state")].empty());
  std::error_code ec;
  std::filesystem::remove(path + ".v15.bak", ec);
}
