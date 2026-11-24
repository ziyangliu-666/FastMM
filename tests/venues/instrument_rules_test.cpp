// RulesWatch (venues/instrument_rules.hpp): an instrument's trading rules read again while the
// session runs. A read that changes nothing emits nothing; one that changes a field emits one
// InstrumentRulesMsg naming the fields; the reads are paced by the period and by refusals.
#include "fastmm/venues/instrument_rules.hpp"

#include "venue_test_util.hpp"

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::test;

namespace {

constexpr std::int64_t kS = 1'000'000'000;
const InstrumentId kBtc{0};
const VenueId kVenue{0};

VenueRules btc_rules() {
  return rules_of(make_instrument("BTCUSDT", 0, "BTC", "USDT"));
}

}  // namespace

TEST_CASE("venues.rules: a read that changes nothing emits nothing, a change emits its fields") {
  RulesWatch w;
  RecordingSink rs(1U << 16, SinkPolicy::Spin);
  CHECK(w.trading(kBtc));                                     // never seeded
  CHECK(w.update(&rs.sink, kVenue, kBtc, btc_rules()) == 0);  // not seeded: ignored
  w.seed(kBtc, btc_rules());
  CHECK(w.seeded(kBtc));
  CHECK(w.update(&rs.sink, kVenue, kBtc, btc_rules()) == 0);
  CHECK(rs.drain().empty());

  // A split: the lot step and the min quantity move.
  VenueRules split = btc_rules();
  split.lot = Qty::from_decimal("0.01").value();
  split.min_qty = split.lot;
  CHECK(w.update(&rs.sink, kVenue, kBtc, split) ==
        (InstrumentRulesMsg::kLot | InstrumentRulesMsg::kMinQty));
  auto msgs = rs.drain();
  REQUIRE(msgs.size() == 1);
  const auto& m = RecordingSink::as<InstrumentRulesMsg>(msgs[0]);
  CHECK(m.hdr.type == EventType::InstrumentRules);
  CHECK(m.hdr.instrument == kBtc);
  CHECK(m.hdr.venue == kVenue);
  CHECK(m.hdr.recv_ts.valid());  // a backtest of the journal orders it by this
  CHECK(m.lot == split.lot);
  CHECK(m.tick == split.tick);
  CHECK(m.status == TradingStatus::Trading);
  CHECK(m.cause == InstrumentRulesMsg::kRefresh);
  CHECK(w.last(kBtc) == split);
  CHECK(w.changes() == 1);

  // A halt, with a reply that has no usable tick: the last tick stays.
  VenueRules halt = split;
  halt.status = TradingStatus::Halted;
  halt.tick = Price{};
  CHECK(w.update(&rs.sink, kVenue, kBtc, halt) == InstrumentRulesMsg::kStatus);
  CHECK_FALSE(w.trading(kBtc));
  msgs = rs.drain();
  REQUIRE(msgs.size() == 1);
  CHECK(RecordingSink::as<InstrumentRulesMsg>(msgs[0]).tick == split.tick);
  CHECK(RecordingSink::as<InstrumentRulesMsg>(msgs[0]).status == TradingStatus::Halted);

  // A pool member reads for trading() only: nothing is emitted, the state follows.
  halt.status = TradingStatus::Trading;
  CHECK(w.update(nullptr, kVenue, kBtc, halt) == InstrumentRulesMsg::kStatus);
  CHECK(w.trading(kBtc));
  CHECK(rs.drain().empty());
}

TEST_CASE("venues.rules: reads are due by the period, and soon after a refusal") {
  RulesWatch w;
  w.configure(60 * kS);
  CHECK_FALSE(w.due(1000 * kS));  // nothing seeded yet
  w.seed(kBtc, btc_rules());
  CHECK(w.due(1000 * kS));
  w.started(1000 * kS);
  CHECK(w.in_flight());
  CHECK_FALSE(w.due(2000 * kS));  // one at a time
  w.finished();
  CHECK_FALSE(w.due(1059 * kS));
  CHECK(w.due(1060 * kS));

  // A refusal: due once kRefusalGapNs has passed since the last read, and the read says why.
  w.want();
  CHECK_FALSE(w.due(1004 * kS));
  CHECK(w.due(1005 * kS));
  w.started(1005 * kS);
  w.finished();
  RecordingSink rs(1U << 16, SinkPolicy::Spin);
  VenueRules halt = btc_rules();
  halt.status = TradingStatus::Closed;
  CHECK(w.update(&rs.sink, kVenue, kBtc, halt) == InstrumentRulesMsg::kStatus);
  const auto msgs = rs.drain();
  REQUIRE(msgs.size() == 1);
  CHECK(RecordingSink::as<InstrumentRulesMsg>(msgs[0]).cause == InstrumentRulesMsg::kRefusal);
  CHECK(w.reads() == 2);

  // Period 0: only refusals read.
  RulesWatch off;
  off.configure(0);
  off.seed(kBtc, btc_rules());
  CHECK_FALSE(off.due(1'000'000 * kS));
  off.want();
  CHECK(off.due(1'000'000 * kS));
}

TEST_CASE("venues.rules: the refusals that read the rules again") {
  CHECK(refusal_reads_rules(RejectReason::InvalidLot));
  CHECK(refusal_reads_rules(RejectReason::InvalidTick));
  CHECK(refusal_reads_rules(RejectReason::BelowMinNotional));
  CHECK(refusal_reads_rules(RejectReason::InstrumentDisabled));
  CHECK_FALSE(refusal_reads_rules(RejectReason::PostOnlyWouldCross));
  CHECK_FALSE(refusal_reads_rules(RejectReason::VenueRateLimit));
}
