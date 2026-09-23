// The SQLite backend: records in, rows out, and the guarantees the interface promises.
#include "fastmm/store/sqlite_store.hpp"

#include "store_test_util.hpp"

#include "fastmm/core/msg_ring.hpp"
#include "fastmm/store/registry.hpp"
#include "fastmm/store/store_thread.hpp"

#include <filesystem>
#include <memory>

using namespace fastmm;
using namespace fastmm::store;
using fastmm::test::kDay1Ns;
using fastmm::test::kDay2Ns;
using fastmm::test::tmp_dir;

namespace {

struct Store {
  GenericSection section;
  BackendOptions opts;
  std::unique_ptr<Backend> backend;

  explicit Store(const std::string& path) {
    section.values["path"] = path;
    opts.config = &section;
    opts.engine_name = "test";
    backend = make_sqlite_backend();
    REQUIRE(backend->open(opts));
  }
};

std::unique_ptr<Reader> open_reader(const std::string& path, GenericSection& section) {
  section.values["path"] = path;
  BackendOptions o;
  o.config = &section;
  o.read_only = true;
  auto r = make_sqlite_reader();
  REQUIRE(r->open(o));
  return r;
}

std::string fresh(const char* name) {
  const auto p = tmp_dir() / name;
  std::error_code ec;
  std::filesystem::remove(p, ec);
  std::filesystem::remove(p.string() + "-wal", ec);
  std::filesystem::remove(p.string() + "-shm", ec);
  return p.string();
}

// Index of `name` in the row header.
std::size_t column(const Rows& r, std::string_view name) {
  for (std::size_t i = 0; i < r.columns.size(); ++i) {
    if (r.columns[i] == name) return i;
  }
  REQUIRE_MESSAGE(false, "no column " << name);
  return 0;
}

}  // namespace

TEST_CASE("store.sqlite: a session round trips through fills, orders, positions and PnL") {
  const std::string path = fresh("roundtrip.db");
  const InstrumentTable table = fastmm::test::store_table();
  {
    Store s(path);
    REQUIRE(s.backend->session_open(fastmm::test::store_session(42, kDay1Ns)));
    REQUIRE(s.backend->instruments(42, std::span<const Instrument>(table.data(), table.size())));
    s.backend->begin();
    s.backend->fill(fastmm::test::store_fill(42,
                                             1,
                                             kDay1Ns + 3600'000'000'000LL,
                                             Side::Buy,
                                             10'000'000'000,
                                             100'000'000,
                                             0,
                                             1'000,
                                             "E1"));
    s.backend->position(fastmm::test::store_position(
        42, 2, kDay1Ns + 3600'000'000'000LL, 100'000'000, 0, 1'000, 1));
    s.backend->fill(fastmm::test::store_fill(42,
                                             3,
                                             kDay1Ns + 7200'000'000'000LL,
                                             Side::Sell,
                                             10'100'000'000,
                                             100'000'000,
                                             100'000'000,
                                             2'000,
                                             "E2"));
    s.backend->position(fastmm::test::store_position(
        42, 4, kDay1Ns + 7200'000'000'000LL, 0, 100'000'000, 2'000, 2));
    // The next day continues the same session: its roll-up gets the change, not the total.
    s.backend->position(
        fastmm::test::store_position(42, 5, kDay2Ns + 60'000'000'000LL, 0, 150'000'000, 3'000, 3));
    s.backend->order(fastmm::test::store_order(42, 6, kDay1Ns, 1, OrderState::Filled));
    s.backend->order(fastmm::test::store_order(42, 7, kDay1Ns, 2, OrderState::Live));
    s.backend->kill(fastmm::test::store_kill(42, 8, kDay2Ns, KillReason::MaxLoss));
    s.backend->commit();
    SessionClose c;
    c.session_id = 42;
    c.stopped_ns = kDay2Ns + 120'000'000'000LL;
    c.exit_code = 6;
    c.kill_reason = KillReason::MaxLoss;
    c.kill_latched = true;
    c.stats.fills = 2;
    c.stats.realized_pnl_raw = 150'000'000;
    c.stats.fees_raw = 3'000;
    c.journal_paths = {"runs/test.fmj", "runs/test.1.fmj"};
    REQUIRE(s.backend->session_close(c));
    CHECK(s.backend->errors() == 0);
    s.backend->close();
  }

  GenericSection section;
  auto reader = open_reader(path, section);

  SUBCASE("sessions") {
    auto r = reader->sessions(QueryFilter{});
    REQUIRE(r);
    REQUIRE(r->rows.size() == 1);
    CHECK(r->rows[0][column(*r, "session_id")] == "42");
    CHECK(r->rows[0][column(*r, "strategy")] == "basic_mm");
    CHECK(r->rows[0][column(*r, "realized")] == "1.5");
    CHECK(r->rows[0][column(*r, "kill_reason")] == "MaxLoss");
    CHECK(r->rows[0][column(*r, "started")] == "2024-03-04 00:00:00");
  }

  SUBCASE("fills of one session") {
    QueryFilter f;
    f.session_id = 42;
    auto r = reader->fills(f);
    REQUIRE(r);
    REQUIRE(r->rows.size() == 2);
    CHECK(r->rows[0][column(*r, "symbol")] == "BTCUSDT");
    CHECK(r->rows[0][column(*r, "side")] == "Buy");
    CHECK(r->rows[0][column(*r, "price")] == "100");
    CHECK(r->rows[0][column(*r, "qty")] == "1");
    CHECK(r->rows[1][column(*r, "side")] == "Sell");
    CHECK(r->rows[1][column(*r, "exec_id")] == "E2");
  }

  SUBCASE("PnL by day is the change within each day, not the running total") {
    auto r = reader->pnl(QueryFilter{});
    REQUIRE(r);
    REQUIRE(r->rows.size() == 2);
    const std::size_t day = column(*r, "day");
    const std::size_t realized = column(*r, "realized");
    const std::size_t fees = column(*r, "fees");
    CHECK(r->rows[0][day] == "2024-03-04");
    CHECK(r->rows[0][realized] == "1");
    CHECK(r->rows[0][fees] == "0.00002");
    CHECK(r->rows[1][day] == "2024-03-05");
    CHECK(r->rows[1][realized] == "0.5");
    CHECK(r->rows[1][fees] == "0.00001");
  }

  SUBCASE("a day filter narrows the PnL") {
    QueryFilter f;
    f.from = "2024-03-05";
    f.to = "2024-03-05";
    auto r = reader->pnl(f);
    REQUIRE(r);
    REQUIRE(r->rows.size() == 1);
    CHECK(r->rows[0][column(*r, "day")] == "2024-03-05");
  }

  SUBCASE("orders keep their last state, one row each") {
    auto r = reader->orders(QueryFilter{});
    REQUIRE(r);
    REQUIRE(r->rows.size() == 2);
    CHECK(r->rows[0][column(*r, "state")] == "Filled");
    CHECK(r->rows[1][column(*r, "state")] == "Live");
  }

  SUBCASE("recovery reports what the session left behind") {
    QueryFilter f;
    f.engine = "test";
    auto r = reader->recovery(f);
    REQUIRE(r);
    CHECK(r->found);
    CHECK(r->session_id == 42);
    CHECK(r->kill_latched);
    CHECK(r->kill_reason == "MaxLoss");
    CHECK(r->clean_shutdown);
    CHECK(r->realized == "1.5");
    CHECK(r->net == "1.49997");
    REQUIRE(r->open_orders.size() == 1);
    CHECK(r->open_orders[0].find("Live") != std::string::npos);
    REQUIRE(r->journals.size() == 2);
    CHECK(r->journals[1] == "runs/test.1.fmj");
    REQUIRE(r->positions.size() == 1);
  }

  SUBCASE("an unknown engine has no recovery") {
    QueryFilter f;
    f.engine = "other";
    auto r = reader->recovery(f);
    REQUIRE(r);
    CHECK_FALSE(r->found);
  }
}

TEST_CASE("store.sqlite: a fill written twice inserts one row") {
  const std::string path = fresh("idempotent.db");
  const InstrumentTable table = fastmm::test::store_table();
  Store s(path);
  REQUIRE(s.backend->session_open(fastmm::test::store_session(1, kDay1Ns)));
  REQUIRE(s.backend->instruments(1, std::span<const Instrument>(table.data(), table.size())));
  const FillRecord f =
      fastmm::test::store_fill(1, 1, kDay1Ns, Side::Buy, 10'000'000'000, 100'000'000, 0, 0, "X1");
  s.backend->begin();
  s.backend->fill(f);
  s.backend->fill(f);  // the same record again: same seq
  FillRecord other = f;
  other.hdr.seq = 2;  // a different record carrying the same exec id
  s.backend->fill(other);
  s.backend->commit();
  CHECK(s.backend->errors() == 0);
  s.backend->close();

  GenericSection section;
  auto reader = open_reader(path, section);
  auto r = reader->fills(QueryFilter{});
  REQUIRE(r);
  CHECK(r->rows.size() == 1);
}

TEST_CASE("store.sqlite: a cancel-replace closes the client order id it superseded") {
  const std::string path = fresh("replaced.db");
  const InstrumentTable table = fastmm::test::store_table();
  Store s(path);
  REQUIRE(s.backend->session_open(fastmm::test::store_session(3, kDay1Ns)));
  REQUIRE(s.backend->instruments(3, std::span<const Instrument>(table.data(), table.size())));
  s.backend->begin();
  s.backend->order(fastmm::test::store_order(3, 1, kDay1Ns, 1, OrderState::Live));
  s.backend->order(fastmm::test::store_order(3, 2, kDay1Ns, 2, OrderState::Live));
  // Order 1 was replaced by order 2, so only order 2 is still open.
  s.backend->order(fastmm::test::store_replaced(3, 3, kDay1Ns + 1000, 1));
  s.backend->commit();
  CHECK(s.backend->errors() == 0);
  s.backend->close();

  GenericSection section;
  auto reader = open_reader(path, section);
  auto all = reader->orders(QueryFilter{});
  REQUIRE(all);
  REQUIRE(all->rows.size() == 2);
  CHECK(all->rows[1][column(*all, "state")] == "Replaced");
  CHECK(all->rows[1][column(*all, "terminal")] == "1");
  // Its price survives the close: the record that closed it belongs to the order that carries on.
  CHECK(all->rows[1][column(*all, "price")] == "100");
  auto rec = reader->recovery(QueryFilter{});
  REQUIRE(rec);
  REQUIRE(rec->found);
  REQUIRE(rec->open_orders.size() == 1);
  CHECK(rec->open_orders[0].find("Live") != std::string::npos);
}

TEST_CASE("store.sqlite: the registry knows sqlite and refuses an unknown name") {
  StoreRegistry local;
  CHECK(local.find("sqlite") == nullptr);
  register_builtin_backends(local);
  REQUIRE(local.find("sqlite") != nullptr);
  CHECK(local.make_backend("sqlite") != nullptr);
  CHECK(local.make_reader("sqlite") != nullptr);
  CHECK(local.make_backend("clickhouse") == nullptr);
  CHECK(local.names() == "none, sqlite");
  // Registering the same pair again changes nothing; "none" is reserved.
  register_builtin_backends(local);
  CHECK(local.entries().size() == 1);
  CHECK(local.add("none", &make_sqlite_backend) == AddResult::Invalid);
  CHECK(local.add("sqlite", &make_sqlite_backend) == AddResult::Conflict);
}

TEST_CASE("store.sqlite: [storage] backend defaults to sqlite and none disables the store") {
  GenericSection s;
  CHECK(configured_backend(s) == "sqlite");
  s.values["backend"] = "none";
  CHECK(configured_backend(s) == kNoBackend);
  s.values["backend"] = "clickhouse";
  CHECK(configured_backend(s) == "clickhouse");
}

TEST_CASE("store.sqlite: the path comes from [storage] path or the journal directory") {
  BackendOptions o;
  o.engine_name = "mm1";
  o.default_dir = "runs";
  CHECK(sqlite_path(o) == "runs/mm1.db");
  GenericSection s;
  s.values["path"] = "/tmp/other.db";
  o.config = &s;
  CHECK(sqlite_path(o) == "/tmp/other.db");
}

TEST_CASE("store.sqlite: the store thread drains the ring into the backend in order") {
  const std::string path = fresh("thread.db");
  const InstrumentTable table = fastmm::test::store_table();
  MsgRing ring(1 << 16);
  auto backend = make_sqlite_backend();
  GenericSection section;
  section.values["path"] = path;
  BackendOptions opts;
  opts.config = &section;
  REQUIRE(backend->open(opts));
  REQUIRE(backend->session_open(fastmm::test::store_session(9, kDay1Ns)));
  REQUIRE(backend->instruments(9, std::span<const Instrument>(table.data(), table.size())));
  StoreThread st(ring, std::move(backend));

  RecordWriter w(&ring, 9);
  for (int i = 0; i < 40; ++i) {
    FillRecord f = fastmm::test::store_fill(9,
                                            0,
                                            kDay1Ns + i * 1'000'000'000LL,
                                            Side::Buy,
                                            10'000'000'000,
                                            100'000'000,
                                            0,
                                            0,
                                            "E" + std::to_string(i));
    CHECK(w.put(f.hdr));
  }
  CHECK(st.drain_once() == 40);
  CHECK(st.stats().records == 40);
  CHECK(st.stats().batches >= 1);
  st.stop();

  GenericSection read_section;
  auto reader = open_reader(path, read_section);
  auto r = reader->fills(QueryFilter{});
  REQUIRE(r);
  CHECK(r->rows.size() == 40);
}

TEST_CASE("store.sqlite: a full ring drops records and counts them, it never blocks") {
  MsgRing ring(1 << 12);  // 4 KiB: room for about 16 fills
  RecordWriter w(&ring, 1);
  std::size_t accepted = 0;
  for (int i = 0; i < 200; ++i) {
    FillRecord f =
        fastmm::test::store_fill(1, 0, kDay1Ns, Side::Buy, 1, 1, 0, 0, "E" + std::to_string(i));
    if (w.put(f.hdr)) ++accepted;
  }
  CHECK(accepted > 0);
  CHECK(accepted < 200);
  CHECK(w.dropped() == 200 - accepted);
  CHECK(w.written() == accepted);
}

TEST_CASE("store.sqlite: a disabled writer accepts everything and costs nothing") {
  RecordWriter w;
  CHECK_FALSE(w.enabled());
  FillRecord f = fastmm::test::store_fill(1, 0, kDay1Ns, Side::Buy, 1, 1, 0, 0, "E");
  CHECK(w.put(f.hdr));
  CHECK(w.written() == 0);
  CHECK(w.dropped() == 0);
}
