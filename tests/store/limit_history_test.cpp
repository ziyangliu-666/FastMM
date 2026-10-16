// The operator's risk limit changes in the SQLite store (schema 11): one limit_changes row per key
// a `fastmm-ctl limits` changed, with the values before and after and whether --force turned one
// off.
#include "store_test_util.hpp"

#include "fastmm/store/sqlite_store.hpp"

#include <sqlite3.h>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::store;
using fastmm::test::kDay1Ns;
using fastmm::test::tmp_dir;

namespace {

LimitsRecord change(std::uint64_t seq, const RiskLimits& before, const RiskLimits& after) {
  LimitsRecord r{};
  r.hdr.len = sizeof r;
  r.hdr.type = RecordType::Limits;
  r.hdr.session_id = 1;
  r.hdr.seq = seq;
  r.hdr.engine_ts = Timestamp{kDay1Ns + static_cast<std::int64_t>(seq) * 1'000'000'000};
  r.before = before;
  r.after = after;
  r.check_balance_before = r.check_balance_after = 1;
  return r;
}

std::vector<std::string> rows(const std::string& path) {
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
  sqlite3_stmt* st = nullptr;
  REQUIRE(sqlite3_prepare_v2(db,
                             "SELECT seq || ' ' || key || ' ' || before || ' ' || after || ' ' || "
                             "forced FROM limit_changes ORDER BY seq, key",
                             -1,
                             &st,
                             nullptr) == SQLITE_OK);
  std::vector<std::string> out;
  while (sqlite3_step(st) == SQLITE_ROW)
    out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)));
  sqlite3_finalize(st);
  sqlite3_close_v2(db);
  return out;
}

}  // namespace

TEST_CASE("store.limits: each limits command is kept with what it changed") {
  const auto p = tmp_dir() / "limits.db";
  std::error_code ec;
  std::filesystem::remove(p, ec);
  std::filesystem::remove(p.string() + "-wal", ec);
  std::filesystem::remove(p.string() + "-shm", ec);
  const std::string path = p.string();
  const InstrumentTable table = fastmm::test::store_table();
  {
    GenericSection section;
    section.values["path"] = path;
    BackendOptions o;
    o.config = &section;
    o.engine_name = "test";
    auto b = make_sqlite_backend();
    REQUIRE(b->open(o));
    REQUIRE(b->session_open(fastmm::test::store_session(1, kDay1Ns)));
    REQUIRE(b->instruments(1, std::span<const Instrument>(table.data(), table.size())));
    RiskLimits a;
    a.max_position = Qty::from_decimal("0.5").value();
    a.max_loss = Notional::from_decimal("100").value();
    a.stale_md = milliseconds(2000);
    RiskLimits c = a;
    c.max_position = Qty::from_decimal("0.25").value();
    c.stale_md = milliseconds(500);
    RiskLimits d = c;
    d.max_loss = Notional{};
    d.stp = false;
    b->begin();
    b->limits(change(1, a, c));
    LimitsRecord off = change(2, c, d);
    off.forced = 1;
    off.check_balance_after = 0;
    b->limits(off);
    b->limits(change(3, d, d));  // named the values it had: still on record
    LimitsRecord und = change(4, d, d);
    und.kind = LimitsRecord::kUnderlying;
    und.underlying = FixedString<8>("BTC");
    und.underlying_before = Qty::from_decimal("2").value().raw;
    und.underlying_after = Qty::from_decimal("1.5").value().raw;
    b->limits(und);
    b->commit();
    CHECK(b->errors() == 0);
    b->close();
  }
  CHECK(rows(path) == std::vector<std::string>{
                          "1 max_position 0.5 0.25 0",
                          "1 stale_md_ms 2000 500 0",
                          "2 check_balance true false 1",
                          "2 max_loss 100 0 1",
                          "2 stp true false 1",
                          "3    0",
                          "4 underlying.BTC.max_net 2 1.5 0",
                      });
}
