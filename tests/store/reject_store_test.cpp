// Refused orders in the SQLite store (schema 9): one row per RejectRecord with the account's
// name, the limit or side that refused it and the budget then; `count` folds repeats, and the
// summary sums them by account, instrument, side and source.
#include "store_test_util.hpp"

#include "fastmm/store/sqlite_store.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::store;
using fastmm::test::kDay1Ns;
using fastmm::test::tmp_dir;

namespace {

RejectRecord reject(std::uint64_t seq,
                    std::int64_t ts_ns,
                    VenueId account,
                    Side side,
                    RejectReason reason,
                    RejectSource source,
                    std::uint32_t folded,
                    std::uint8_t flags = 0) {
  RejectRecord r{};
  r.hdr.len = sizeof r;
  r.hdr.type = RecordType::Reject;
  r.hdr.session_id = 1;
  r.hdr.seq = seq;
  r.hdr.engine_ts = Timestamp{ts_ns};
  r.hdr.instrument = InstrumentId{0};
  r.hdr.venue = account;
  r.side = side;
  r.reason = reason;
  r.source = source;
  r.folded = folded;
  r.flags = flags;
  r.price = Price::from_decimal("100").value();
  r.qty = Qty::from_decimal("0.01").value();
  r.local_tokens = source == RejectSource::RiskBucket ? 0 : 12;
  r.local_wait_ns = source == RejectSource::RiskBucket ? 20'000'000 : 0;
  r.orders_10s_used = 90;
  r.orders_10s_admits = 90;
  r.budget_known = 1;
  return r;
}

}  // namespace

TEST_CASE("store.rejects: who refused which order, and the sums by account and source") {
  const auto p = tmp_dir() / "rejects.db";
  std::error_code ec;
  std::filesystem::remove(p, ec);
  std::filesystem::remove(p.string() + "-wal", ec);
  std::filesystem::remove(p.string() + "-shm", ec);
  const InstrumentTable table = fastmm::test::store_table();
  {
    GenericSection section;
    section.values["path"] = p.string();
    BackendOptions o;
    o.config = &section;
    auto b = make_sqlite_backend();
    REQUIRE(b->open(o));
    SessionOpen s = fastmm::test::store_session(1, kDay1Ns);
    s.venues = {"binance", "binance-b"};
    REQUIRE(b->session_open(s));
    REQUIRE(b->instruments(1, std::span<const Instrument>(table.data(), table.size())));
    b->begin();
    // Account b's window refused 1 + 4 buys; the bucket refused one sell that only reduced the
    // position; the venue refused one buy on the primary.
    b->reject(reject(1,
                     kDay1Ns + 1'000,
                     VenueId{1},
                     Side::Buy,
                     RejectReason::RateLimit,
                     RejectSource::AccountOrders10s,
                     0));
    b->reject(reject(2,
                     kDay1Ns + 200'000'000,
                     VenueId{1},
                     Side::Buy,
                     RejectReason::RateLimit,
                     RejectSource::AccountOrders10s,
                     4));
    b->reject(reject(3,
                     kDay1Ns + 300'000'000,
                     VenueId{1},
                     Side::Sell,
                     RejectReason::RateLimit,
                     RejectSource::RiskBucket,
                     0,
                     RejectRecord::kReduces));
    RejectRecord venue = reject(4,
                                kDay1Ns + 400'000'000,
                                VenueId{0},
                                Side::Buy,
                                RejectReason::VenueRateLimit,
                                RejectSource::Venue,
                                0,
                                RejectRecord::kFromVenue);
    venue.venue_code = -1015;
    venue.text = FixedString<40>("Too many new orders");
    b->reject(venue);
    b->commit();
    b->close();
  }
  GenericSection section;
  section.values["path"] = p.string();
  BackendOptions o;
  o.config = &section;
  o.read_only = true;
  auto reader = make_sqlite_reader();
  REQUIRE(reader->open(o));
  QueryFilter f;
  auto rows = reader->rejects(f);
  REQUIRE(rows);
  REQUIRE(rows->rows.size() == 4);
  const auto col = [&](std::string_view name) {
    for (std::size_t i = 0; i < rows->columns.size(); ++i)
      if (rows->columns[i] == name) return i;
    FAIL("no column " << name);
    return std::size_t{0};
  };
  CHECK(rows->rows[0][col("account")] == "binance-b");
  CHECK(rows->rows[0][col("source")] == "account_orders_10s");
  CHECK(rows->rows[0][col("orders_10s")] == "90/90");
  CHECK(rows->rows[1][col("count")] == "5");
  CHECK(rows->rows[2][col("source")] == "risk_bucket");
  CHECK(rows->rows[2][col("reduces")] == "1");
  CHECK(rows->rows[2][col("tokens")] == "0");
  CHECK(rows->rows[2][col("token_wait_us")] == "20000");
  CHECK(rows->rows[3][col("venue_code")] == "-1015");
  CHECK(rows->rows[3][col("text")] == "Too many new orders");

  auto sum = reader->reject_summary(f);
  REQUIRE(sum);
  std::map<std::string, std::string> by;
  for (const auto& r : sum->rows) by[r[0] + "/" + r[2] + "/" + r[5]] = r[6];
  CHECK(by == std::map<std::string, std::string>{{"binance-b/Buy/account_orders_10s", "6"},
                                                 {"binance-b/Sell/risk_bucket", "1"},
                                                 {"binance/Buy/venue", "1"}});
}
