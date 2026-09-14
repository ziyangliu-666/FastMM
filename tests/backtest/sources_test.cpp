// Data sources: CSV parsing, array validation, journal round trip, k-way merge, and the same
// data through every source giving identical backtest results.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/array_source.hpp"
#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/csv_source.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/synthetic_source.hpp"

#include <cstring>
#include <stdexcept>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {
const char* kTape =
    "ts_ns,type,inst,side,price,qty,seq\n"
    "# comment\n"
    "100,S,0,B,99.5,2,7\n"
    "100,S,0,B,99.4,3,7\n"
    "100,S,0,A,100.5,1.25,7\n"
    "\n"
    "150,D,0,A,100.5,0,8\n"
    "150,D,0,A,100.6,4,8\n"
    "200,T,0,B,100.6,0.5,99\n"
    "250,B,0,B,99.5,2,9\n"
    "250,B,0,A,100.6,3.5,9\n";

std::vector<std::vector<std::byte>> drain(MdSource& s) {
  std::vector<std::vector<std::byte>> out;
  while (const EventHeader* h = s.next()) {
    std::vector<std::byte> b(h->len);
    std::memcpy(b.data(), h, h->len);
    reinterpret_cast<EventHeader*>(b.data())->seq = 0;  // journal-assigned
    out.push_back(std::move(b));
  }
  return out;
}
}  // namespace

TEST_CASE("backtest.csv: rows group into snapshot, delta, trade and ticker messages") {
  CsvSource src = CsvSource::from_text(kTape);
  CHECK(src.rows() == 8);
  CHECK(src.start_ts() == Timestamp{100});
  const EventHeader* h = src.next();
  REQUIRE(h != nullptr);
  CHECK(h->type == EventType::BookSnapshot);
  const auto& snap = msg_cast<BookDeltaMsg>(h);
  CHECK(snap.is_snapshot());
  REQUIRE(snap.bid_count == 2);
  REQUIRE(snap.ask_count == 1);
  CHECK(snap.bids()[1] == Level{px("99.4"), qt("3")});
  CHECK(snap.asks()[0] == Level{px("100.5"), qt("1.25")});
  CHECK(snap.last_update_id == 7);
  CHECK(snap.hdr.exch_ts == Timestamp{100});

  h = src.next();
  REQUIRE(h != nullptr);
  const auto& d = msg_cast<BookDeltaMsg>(h);
  CHECK(h->type == EventType::BookDelta);
  CHECK(d.bid_count == 0);
  REQUIRE(d.ask_count == 2);
  CHECK(d.asks()[0].qty.is_zero());

  h = src.next();
  REQUIRE(h != nullptr);
  REQUIRE(h->type == EventType::Trade);
  CHECK(msg_cast<TradeMsg>(h).aggressor == Side::Buy);
  CHECK(msg_cast<TradeMsg>(h).trade_id == 99);

  h = src.next();
  REQUIRE(h != nullptr);
  REQUIRE(h->type == EventType::BookTicker);
  CHECK(msg_cast<BookTickerMsg>(h).bid_px == px("99.5"));
  CHECK(msg_cast<BookTickerMsg>(h).ask_qty == qt("3.5"));
  CHECK(src.next() == nullptr);

  src.reset();
  CHECK(drain(src).size() == 4);
}

TEST_CASE("backtest.csv: malformed input is rejected with the line number") {
  Row r;
  CHECK_FALSE(CsvSource::parse_row("ts_ns,type,inst,side,price,qty,seq", r));
  CHECK_FALSE(CsvSource::parse_row("   ", r));
  CHECK(CsvSource::parse_row("5,T,1,sell,1.5,2,3", r));
  CHECK(r.side == Side::Sell);
  CHECK(r.inst == 1);
  CHECK_THROWS_AS(CsvSource::parse_row("5,X,0,B,1,1,1", r), std::runtime_error);
  CHECK_THROWS_AS(CsvSource::parse_row("5,T,0,B,1,1", r), std::runtime_error);
  CHECK_THROWS_AS(CsvSource::parse_row("5,T,0,Q,1,1,1", r), std::runtime_error);
  CHECK_THROWS_AS(CsvSource::parse_row("5,T,0,B,1.000000001,1,1", r), std::runtime_error);
  CHECK_THROWS_AS(CsvSource::parse_row("5,T,0,B,1,1,99999999999999999999", r), std::runtime_error);
  try {
    static_cast<void>(
        CsvSource::from_text("ts_ns,type,inst,side,price,qty,seq\n1,T,0,B,1,1,1\n0,T,0,B,1,1,1\n"));
    FAIL("decreasing timestamps accepted");
  } catch (const std::runtime_error& e) {
    CHECK(std::string(e.what()).find("line 3") != std::string::npos);
  }
  CHECK_THROWS_AS(CsvSource("/nonexistent/file.csv"), std::runtime_error);
}

TEST_CASE("backtest.array: int64 and float64 columns equal the CSV stream, bad columns throw") {
  CsvSource csv = CsvSource::from_text(kTape);
  const auto want = drain(csv);
  const OwnedColumns cols = OwnedColumns::from_csv(kTape);
  ArraySource ai(cols.view(false));
  ArraySource af(cols.view(true));
  CHECK(ai.rows() == 8);
  CHECK(ai.start_ts() == Timestamp{100});
  CHECK(drain(ai) == want);
  CHECK(drain(af) == want);
  ai.reset();
  CHECK(drain(ai) == want);

  ArrayColumns no_seq = cols.view(false);
  no_seq.seq = {};
  ArraySource ns(no_seq);
  CHECK(drain(ns).size() == want.size());  // grouping by (type, ts, inst) still holds here

  ArrayColumns bad = cols.view(false);
  bad.price_f64 = cols.price_f;  // both price columns
  CHECK_THROWS_AS(ArraySource{bad}, std::invalid_argument);
  bad = cols.view(false);
  bad.inst = std::span<const std::uint32_t>(cols.inst.data(), 3);
  CHECK_THROWS_AS(ArraySource{bad}, std::invalid_argument);
  OwnedColumns wrong = cols;
  wrong.type[2] = 9;
  CHECK_THROWS_AS(ArraySource{wrong.view(false)}, std::invalid_argument);
  wrong = cols;
  wrong.ts[5] = 0;
  CHECK_THROWS_AS(ArraySource{wrong.view(false)}, std::invalid_argument);
  CHECK(ArrayColumns{}.validate().empty());  // zero rows is fine
}

TEST_CASE("backtest.journal_source: market data round-trips through an .fmj file") {
  const BacktestConfig cfg = BacktestConfig::single_instrument("X", px("0.01"), qt("0.001"));
  CsvSource csv = CsvSource::from_text(kTape);
  const auto want = drain(csv);
  const std::string path = (fastmm::test::tmp_dir() / "sources_roundtrip.fmj").string();
  CHECK(write_md_journal(csv, path, cfg.instruments, 7) == 4);
  JournalSource js(path);
  CHECK(js.md_events() == 4);
  CHECK(js.start_ts() == Timestamp{100});
  CHECK(js.reader().instruments().size() == 1);
  CHECK(drain(js) == want);
  js.reset();
  CHECK(drain(js) == want);
  CHECK_THROWS_AS(JournalSource("/nonexistent/x.fmj"), std::runtime_error);
  CHECK(write_md_journal(csv, path, cfg.instruments, 7, "data", 2) == 2);
  CHECK(JournalSource(path).md_events() == 2);
}

TEST_CASE("backtest.merge: k-way merge is time ordered and stable on ties") {
  CsvSource a = CsvSource::from_text("1,T,0,B,1,1,1\n3,T,0,B,1,1,3\n5,T,0,B,1,1,5\n");
  CsvSource b = CsvSource::from_text("2,T,1,B,1,1,2\n3,T,1,B,1,1,33\n4,T,1,B,1,1,4\n");
  MergedSource m({&a, &b});
  CHECK(m.start_ts() == Timestamp{1});
  std::vector<std::uint64_t> ids;
  while (const EventHeader* h = m.next()) ids.push_back(msg_cast<TradeMsg>(h).trade_id);
  CHECK(ids == std::vector<std::uint64_t>{1, 2, 3, 33, 4, 5});
  m.reset();
  CHECK(m.next() != nullptr);
}

TEST_CASE("backtest.sources: synthetic, journal, CSV and array data give identical backtests") {
  BacktestConfig cfg = synthetic_config(21, seconds(8));
  cfg.transport.fill_model = sim::FillModel::L2Queue;
  cfg.transport.queue_conservatism_bps = 5000;
  SyntheticSourceConfig sc;
  sc.generator = cfg.generator;
  sc.seed = 21;
  sc.duration = seconds(8);
  SyntheticSource synth(sc);
  const std::string text = source_to_csv(synth);
  const std::string path = (fastmm::test::tmp_dir() / "sources_equivalence.fmj").string();
  const std::uint64_t n = write_md_journal(synth, path, cfg.instruments, 21);
  CHECK(synth.dropped() == 0);
  REQUIRE(n > 200);

  const BacktestResult base = run_backtest(cfg, "basic_mm", &synth);
  REQUIRE(base.metrics.fills > 0);
  CHECK(base.md_events == n);

  JournalSource js(path);
  CsvSource csv = CsvSource::from_text(text);
  const OwnedColumns cols = OwnedColumns::from_csv(text);
  ArraySource ai(cols.view(false));
  ArraySource af(cols.view(true));
  const MdSource* sources[] = {&js, &csv, &ai, &af};
  const char* names[] = {"journal", "csv", "array-i64", "array-f64"};
  for (int i = 0; i < 4; ++i) {
    CAPTURE(names[i]);
    const BacktestResult r = run_backtest(cfg, "basic_mm", const_cast<MdSource*>(sources[i]));
    CHECK(r.outbound_sha256 == base.outbound_sha256);
    CHECK(r.outbound_messages == base.outbound_messages);
    CHECK(r.md_events == base.md_events);
    CHECK(r.fills.price == base.fills.price);
    CHECK(r.fills.ts == base.fills.ts);
    CHECK(r.equity.realized == base.equity.realized);
    CHECK(r.equity.unrealized == base.equity.unrealized);
    CHECK(r.equity.position == base.equity.position);
  }
}

TEST_CASE("backtest.journal_source: OptionTicker is market data and round-trips") {
  CHECK(JournalSource::is_market_data(EventType::OptionTicker));
  struct TickerSource final : sim::MdSource {
    OptionTickerMsg m{};
    int left = 1;
    TickerSource() {
      init_header(m, EventType::OptionTicker, InstrumentId{0}, VenueId{0});
      m.hdr.recv_ts = Timestamp{100};
      m.hdr.exch_ts = Timestamp{100};
      m.mark_iv = 0.312;
      m.underlying_price = px("76904.4");
    }
    const EventHeader* next() override { return left-- > 0 ? &m.hdr : nullptr; }
    void reset() override { left = 1; }
    [[nodiscard]] Timestamp start_ts() const override { return Timestamp{100}; }
  } src;
  const BacktestConfig cfg = BacktestConfig::single_instrument("X", px("0.01"), qt("0.001"));
  const std::string path = (fastmm::test::tmp_dir() / "option_ticker.fmj").string();
  CHECK(write_md_journal(src, path, cfg.instruments, 7) == 1);
  JournalSource js(path);
  CHECK(js.md_events() == 1);
  const EventHeader* h = js.next();
  REQUIRE(h != nullptr);
  CHECK(h->type == EventType::OptionTicker);
  CHECK(msg_cast<OptionTickerMsg>(h).mark_iv == doctest::Approx(0.312));
  CHECK(msg_cast<OptionTickerMsg>(h).underlying_price == px("76904.4"));
}
