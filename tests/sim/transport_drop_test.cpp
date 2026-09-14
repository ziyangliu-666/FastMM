// A transport that refuses messages: the journal records them after the hand-off, marked dropped,
// and a replay of that journal refuses the same messages, so both runs trip the kill switch alike.
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/sim/journal_feed.hpp"
#include "fastmm/sim/outbound_hash.hpp"
#include "fastmm/sim/replay_transport.hpp"
#include "fastmm/sim/sim_driver.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace fastmm;
using fastmm::test::tmp_dir;

namespace {

// Accepts the first `capacity` messages of the whole run, then refuses everything.
class LimitedTransport {
 public:
  explicit LimitedTransport(std::size_t capacity) noexcept : capacity_(capacity) {}
  [[nodiscard]] bool send(const EventHeader& m) noexcept {
    const EventHeader* one[1] = {&m};
    return send(std::span<const EventHeader* const>(one, 1)) == 1;
  }
  [[nodiscard]] std::size_t send(std::span<const EventHeader* const> batch) noexcept {
    std::size_t ok = 0;
    for (const EventHeader* m : batch) {
      if (accepted_ >= capacity_) break;
      hasher_.add(*m);
      ++accepted_;
      ++ok;
    }
    return ok;
  }
  [[nodiscard]] bool supports_replace(VenueId) const noexcept { return false; }
  [[nodiscard]] std::string hash() const { return hasher_.hex(); }
  [[nodiscard]] std::uint64_t accepted() const noexcept { return accepted_; }

 private:
  std::size_t capacity_;
  std::size_t accepted_ = 0;
  sim::OutboundHasher hasher_;
};

InstrumentTable table() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.venue = VenueId{0};
  i.flags = Instrument::kEnabled;
  i.tick = Price::from_decimal("0.01").value();
  i.lot = Qty::from_decimal("0.001").value();
  i.min_qty = i.lot;
  REQUIRE(t.add(i));
  return t;
}

void push_book(InlineFeed& feed, SimClock& clock, const char* bid, const char* ask) {
  alignas(64) std::byte buf[BookDeltaMsg::size_for(1, 1)];
  auto* d = reinterpret_cast<BookDeltaMsg*>(buf);
  init_header(*d, EventType::BookSnapshot, InstrumentId{0}, VenueId{0}, sizeof buf);
  d->hdr.flags |= EventHeader::kSnapshot;
  d->hdr.recv_ts = clock.now();
  d->bid_count = d->ask_count = 1;
  d->levels()[0] = Level{Price::from_decimal(bid).value(), Qty::from_decimal("1").value()};
  d->levels()[1] = Level{Price::from_decimal(ask).value(), Qty::from_decimal("1").value()};
  REQUIRE(feed.push(d->hdr));
}

EngineConfig engine_config() {
  EngineConfig ec;
  ec.quotes.min_requote_interval = Duration{};
  ec.risk.max_order_qty = Qty::from_decimal("1").value();
  ec.risk.max_position = Qty::from_decimal("10").value();
  ec.risk.max_open_orders = 8;
  return ec;
}

ParamMap params() {
  return {{"half_spread_bps", "10"},
          {"skew_bps_per_unit", "0"},
          {"quote_qty", "0.01"},
          {"max_inventory", "1"},
          {"requote_threshold_ticks", "1"},
          {"pull_on_stale_ms", "0"},
          {"levels", "2"}};
}

}  // namespace

TEST_CASE("sim.replay: messages the transport refuses are journaled as dropped and replay alike") {
  const auto path = (tmp_dir() / "transport_drop.fmj").string();
  const InstrumentTable instruments = table();
  EngineStats recorded_stats;
  std::string recorded_hash;
  {
    MsgRing ring(1 << 20);
    JournalSessionInfo info;
    info.strategy = "basic_mm";
    info.instruments = &instruments;
    info.has_session = true;
    info.session_epoch = 1;
    JournalFileWriter fw(ring, path, info);
    REQUIRE(fw.ok());
    SimClock clock(Timestamp{1'700'000'000'000'000'000LL});
    LimitedTransport transport(3);  // two levels per side: the 4th order is refused
    InlineFeed feed(1 << 16);
    BasicMM strategy;
    REQUIRE_FALSE(strategy.configure(params()));
    Engine<BasicMM, SimClock, LimitedTransport, InlineFeed> engine(
        engine_config(), instruments, clock, transport, feed, strategy, &ring);
    engine.warm_up();
    engine.start();
    push_book(feed, clock, "100.00", "100.02");
    clock.advance(milliseconds(1));
    engine.step();
    engine.finish();
    fw.drain_once();
    fw.stop();
    recorded_stats = engine.stats();
    recorded_hash = transport.hash();
    CHECK(transport.accepted() == 3);
  }
  CHECK(recorded_stats.transport_full >= 1);
  CHECK(recorded_stats.kills == 1);

  JournalReader reader;
  REQUIRE(reader.open(path));
  // The quote batch: three orders accepted, the fourth refused; everything after it (the kill
  // switch's cancels) is refused as well.
  std::vector<bool> marks;
  reader.for_each([&](const EventHeader* h) {
    if ((h->flags & EventHeader::kOutbound) != 0)
      marks.push_back((h->flags & EventHeader::kDropped) != 0);
  });
  REQUIRE(marks.size() >= 4);
  CHECK_FALSE(marks[0]);
  CHECK_FALSE(marks[1]);
  CHECK_FALSE(marks[2]);
  for (std::size_t i = 3; i < marks.size(); ++i) CHECK(marks[i]);
  const std::uint64_t out = marks.size();
  const std::uint64_t dropped = out - 3;

  // Replay: the same refusals, the same kill switch, the same accepted stream.
  sim::ReplayTransport rt;
  rt.set_expected(sim::ReplayTransport::load_outbound(reader));
  rt.set_dropped(sim::ReplayTransport::load_dropped(reader));
  sim::JournalFeed jf(reader);
  SimClock rclock(Timestamp{reader.header().start_ts_ns});
  BasicMM rstrategy;
  REQUIRE_FALSE(rstrategy.configure(params()));
  Engine<BasicMM, SimClock, sim::ReplayTransport, sim::JournalFeed> replay(
      engine_config(), instruments, rclock, rt, jf, rstrategy);
  sim::ReplayDriver driver(rclock, jf, sim::EngineHooks::for_engine(replay));
  driver.run_all();
  driver.finish();
  CHECK(rt.first_mismatch() == -1);
  CHECK(rt.attempts() == out);
  CHECK(rt.count() == out - dropped);
  CHECK(rt.hash_hex() == recorded_hash);
  CHECK(replay.stats().transport_full == recorded_stats.transport_full);
  CHECK(replay.stats().kills == recorded_stats.kills);
}
