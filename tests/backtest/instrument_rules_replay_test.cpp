// A venue's trading rules changing during a run (InstrumentRulesMsg) are inputs like any other: the
// journal keeps them where the engine took them, and a replay of it sends what the run sent. A run
// that never saw them sends something else.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/data_source.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/backtest/synthetic_source.hpp"
#include "fastmm/core/journal.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

// The rules changes, at their times.
class RulesSource final : public MdSource {
 public:
  explicit RulesSource(std::vector<InstrumentRulesMsg> msgs) : msgs_(std::move(msgs)) {}
  const EventHeader* next() override { return at_ < msgs_.size() ? &msgs_[at_++].hdr : nullptr; }
  void reset() override { at_ = 0; }

 private:
  std::vector<InstrumentRulesMsg> msgs_;
  std::size_t at_ = 0;
};

InstrumentRulesMsg change(Timestamp at,
                          const char* lot,
                          TradingStatus status,
                          std::uint8_t changed) {
  InstrumentRulesMsg m{};
  init_header(m, EventType::InstrumentRules, InstrumentId{0}, VenueId{0});
  m.hdr.exch_ts = at;
  m.hdr.recv_ts = at;
  m.tick = px("0.01");
  m.lot = qt(lot);
  m.min_qty = qt(lot);
  m.status = status;
  m.changed = changed;
  return m;
}

}  // namespace

TEST_CASE("backtest.rules: rules changes are journaled and replay to the recorded hash") {
  BacktestConfig cfg = synthetic_config(41, seconds(10));
  cfg.strategy = "basic_mm";
  const auto path = fastmm::test::tmp_dir() / "instrument_rules.fmj";
  std::filesystem::remove(path);
  cfg.journal_out = path.string();

  SyntheticSource market(synthetic_source_config(cfg));
  const Timestamp t0 = market.start_ts();
  constexpr std::uint8_t kLot = InstrumentRulesMsg::kLot | InstrumentRulesMsg::kMinQty;
  RulesSource rules(
      {change(t0 + seconds(2), "0.001", TradingStatus::Trading, kLot),
       change(t0 + seconds(4), "0.001", TradingStatus::Halted, InstrumentRulesMsg::kStatus),
       change(t0 + seconds(7), "0.001", TradingStatus::Trading, InstrumentRulesMsg::kStatus)});
  MergedSource merged({&market, &rules});
  const BacktestResult rec = run_backtest(cfg, "basic_mm", &merged);
  REQUIRE(rec.outbound_messages > 20);

  JournalReader reader;
  REQUIRE(reader.open(cfg.journal_out));
  CHECK(reader.version() == kJournalVersion);
  std::size_t records = 0;
  reader.for_each([&](const EventHeader* h) {
    if (h->type != EventType::InstrumentRules) return;
    ++records;
    CHECK((h->flags & EventHeader::kEngineTime) != 0);
  });
  CHECK(records == 3);

  // The replay sends what the journal says the run sent: the orders, and the reconciliation the
  // engine asked for when trading resumed, which the simulated venue does not count as an order.
  const ReplayResult rp = replay_journal(cfg.journal_out, cfg);
  CHECK(rp.ok());
  CHECK(rp.outbound_sha256 == rp.recorded_sha256);
  CHECK(rp.outbound_messages == rec.outbound_messages + 1);

  // Without the changes the run quotes on through the halt with the old lot: other orders.
  BacktestConfig plain = cfg;
  plain.journal_out.clear();
  market.reset();
  CHECK(run_backtest(plain, "basic_mm", &market).outbound_sha256 != rec.outbound_sha256);
}
