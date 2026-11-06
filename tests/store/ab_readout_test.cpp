// fastmm-pnl ab (store/ab_readout.hpp) on a small store: two instruments switching arms each UTC
// day over three days, fills with markouts, the arms' instrument-hours from the assignments.
#include "fastmm/store/ab_readout.hpp"

#include "store_test_util.hpp"

#include "fastmm/store/sqlite_store.hpp"

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

using namespace fastmm;
using namespace fastmm::store;
using fastmm::test::kDay1Ns;
using fastmm::test::tmp_dir;

namespace {

constexpr std::int64_t kDayNs = 86'400'000'000'000LL;
constexpr std::int64_t kHourNs = 3'600'000'000'000LL;
constexpr std::int64_t kPx = 10'000'000'000;  // 100

ParamRecord arm_update(std::uint64_t seq, std::int64_t ts, std::uint8_t arm, InstrumentId inst) {
  ParamRecord r{};
  r.hdr.len = sizeof r;
  r.hdr.type = RecordType::Param;
  r.hdr.session_id = 1;
  r.hdr.seq = seq;
  r.hdr.engine_ts = Timestamp{ts};
  r.hdr.instrument = inst;
  r.origin = ParamUpdateMsg::Origin::Experiment;
  r.arm = arm;
  r.publish_seq = seq;
  std::strncpy(r.source, "ab:t", sizeof r.source - 1);
  r.count = 1;
  r.field[0] = 0;
  r.value[0] = arm == 2 ? 80'000 : 50'000;
  return r;
}

MarkoutRecord markout(std::uint64_t fill_seq, std::int64_t ts, std::int32_t h, std::int64_t mid) {
  MarkoutRecord r{};
  r.hdr.len = sizeof r;
  r.hdr.type = RecordType::Markout;
  r.hdr.session_id = 1;
  r.hdr.engine_ts = Timestamp{ts + std::int64_t{h} * 1'000'000};
  r.fill_seq = fill_seq;
  r.fill_ts = Timestamp{ts};
  r.horizon_ms = h;
  r.mid = Price::from_raw(mid);
  return r;
}

// Day d (0, 1, 2): instrument 0 is in control on days 0 and 2 and wide on day 1, instrument 1 the
// other way round. Each instrument buys 1 at 100 each day; a wide fill is worth 0.01 more at 1 s
// than a control fill (0.016 on day 2), plus d x 0.001 for the day's market, and twice that at 60
// s.
std::string build_store() {
  const auto p = tmp_dir() / "ab_readout.db";
  std::error_code ec;
  for (const char* suffix : {"", "-wal", "-shm"}) std::filesystem::remove(p.string() + suffix, ec);
  const InstrumentTable table = fastmm::test::store_table();
  GenericSection section;
  section.values["path"] = p.string();
  BackendOptions o;
  o.config = &section;
  o.engine_name = "test";
  auto b = make_sqlite_backend();
  REQUIRE(b->open(o));
  SessionOpen s = fastmm::test::store_session(1, kDay1Ns);
  s.param_table = {{"half_spread_bps", 4}};
  s.params = {{"half_spread_bps", "5", 50'000, true}};
  s.params_complete = true;
  s.ab_test = "t";
  s.ab_arms = {{"control", ""}, {"wide", "half_spread_bps=8"}};
  REQUIRE(b->session_open(s));
  REQUIRE(b->instruments(1, std::span<const Instrument>(table.data(), table.size())));
  b->begin();
  std::uint64_t seq = 1;
  for (int d = 0; d < 3; ++d) {
    const std::int64_t day = kDay1Ns + d * kDayNs;
    const std::uint8_t a0 = d == 1 ? 2 : 1;
    const std::uint8_t a1 = d == 1 ? 1 : 2;
    b->param(arm_update(seq++, day, a0, InstrumentId{0}));
    b->param(arm_update(seq++, day, a1, InstrumentId{1}));
    for (std::uint32_t i = 0; i < 2; ++i) {
      const std::uint8_t arm = i == 0 ? a0 : a1;
      const std::int64_t ts = day + 6 * kHourNs;
      FillRecord f = fastmm::test::store_fill(
          1, seq, ts, Side::Buy, kPx, 100'000'000, 0, 0, "E" + std::to_string(seq));
      f.hdr.instrument = InstrumentId{i};
      f.arm = arm;
      b->fill(f);
      const std::int64_t edge = (arm == 2 ? (d == 2 ? 1'600'000 : 1'000'000) : 0) + d * 100'000;
      b->markout(markout(seq, ts, 0, kPx));
      b->markout(markout(seq, ts, 1000, kPx + edge));
      b->markout(markout(seq, ts, 60'000, kPx + 2 * edge));
      ++seq;
    }
  }
  b->commit();
  SessionClose c;
  c.session_id = 1;
  c.stopped_ns = kDay1Ns + 3 * kDayNs;
  REQUIRE(b->session_close(c));
  b->close();
  return p.string();
}

AbData read(const std::string& path, const QueryFilter& f = {}) {
  GenericSection section;
  section.values["path"] = path;
  BackendOptions o;
  o.config = &section;
  o.read_only = true;
  auto reader = make_sqlite_reader();
  REQUIRE(reader->open(o));
  auto d = reader->ab_data(f, "");
  REQUIRE(d);
  return *d;
}

}  // namespace

TEST_CASE("store.ab_readout: arms, markouts and day-clustered differences from the store") {
  const std::string path = build_store();
  const AbData data = read(path);
  CHECK(data.test == "t");
  REQUIRE(data.arms == std::vector<std::string>{"control", "wide"});
  CHECK(data.horizons_ms == std::vector<std::int32_t>{1000, 60'000});
  REQUIRE(data.fills.size() == 6);

  const AbReadout r = ab_readout(data, {});
  REQUIRE(r.arms.size() == 2);
  CHECK(r.days == 3);
  // Each arm ran one instrument for 24 h on each of three days.
  CHECK(r.arms[0].inst_hours == doctest::Approx(72.0));
  CHECK(r.arms[1].inst_hours == doctest::Approx(72.0));
  CHECK(r.arms[0].fills == 3);
  CHECK(r.arms[1].notional == doctest::Approx(300.0));
  // Wide: 0.01 + 0.01 + 0.016 + (0 + 1 + 2) x 0.001 at 1 s; control: the market part alone.
  CHECK(r.arms[1].pnl[0] == doctest::Approx(0.039));
  CHECK(r.arms[0].pnl[0] == doctest::Approx(0.003));
  CHECK(r.arms[1].markout_bps[1] == doctest::Approx(0.078 / 300.0 * 1e4));

  const auto find = [&](std::string_view metric) {
    for (const AbComparison& c : r.comparisons)
      if (c.metric == metric) return c;
    FAIL("no metric ", metric);
    return AbComparison{};
  };
  // Each instrument against itself: instrument 0 made 0.011 in its wide day against 0.002 over
  // two control days, instrument 1 0.028 over two wide days against 0.001 in its control day.
  CHECK(r.paired);
  const AbComparison pnl = find("markout pnl/hour 1s");
  CHECK(pnl.diff == doctest::Approx((0.011 / 24 - 0.002 / 48 + 0.028 / 48 - 0.001 / 24) / 2));
  CHECK(pnl.lo <= pnl.diff);
  CHECK(pnl.hi >= pnl.diff);
  CHECK(pnl.hi > pnl.lo);
  CHECK(pnl.lo > 0.0);
  CHECK(find("fills/hour").diff == doctest::Approx(0.0));

  // The same store gives the same readout.
  const AbReadout again = ab_readout(read(path), {});
  CHECK(format_ab_readout(again, data) == format_ab_readout(r, data));
  const std::string text = format_ab_readout(r, data);
  CHECK(text.find("test     t: 1 session(s), 3 UTC day(s)") == 0);
  CHECK(text.find("wide             markout pnl/hour 1s") != std::string::npos);

  // One horizon, one day: no interval to draw.
  AbReadoutOptions one;
  one.horizons_ms = {60'000};
  QueryFilter day;
  day.from = day.to = "2024-03-05";
  const AbData d2 = read(path, day);
  CHECK(d2.fills.size() == 2);
  const AbReadout r2 = ab_readout(d2, one);
  CHECK(r2.horizons_ms == std::vector<std::int32_t>{60'000});
  CHECK(r2.arms[1].pnl[0] == doctest::Approx(0.022));
  CHECK(r2.arms[0].inst_hours == doctest::Approx(24.0));

  const std::string csv = ab_daily_csv(r, data);
  CHECK(csv.find("day,arm,inst_hours,fills,notional,fees,markout_pnl_1s,markout_pnl_1m\n") == 0);
  CHECK(csv.find("2024-03-04,wide,24.000000,1,") != std::string::npos);

  // A rebate paid apart from the fills: 1 bp of 300 notional per arm.
  AbReadoutOptions rebate;
  rebate.maker_rebate_bps = 1.0;
  CHECK(ab_readout(data, rebate).arms[0].fees == doctest::Approx(-0.03));

  AbReadoutOptions missing;
  missing.horizons_ms = {5000};
  CHECK_THROWS_AS(static_cast<void>(ab_readout(data, missing)), std::invalid_argument);
}
