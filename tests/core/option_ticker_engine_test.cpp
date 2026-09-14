// EventType::OptionTicker through the engine: dispatched to the optional on_option_ticker hook
// (strategies without it still compile and run), and recorded by the journal byte for byte.
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"

#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

using namespace fastmm;

namespace {

struct NullTransport {
  bool send(const EventHeader&) noexcept { return true; }
  std::size_t send(std::span<const EventHeader* const> b) noexcept { return b.size(); }
  bool supports_replace(VenueId) const noexcept { return false; }
};

struct TickerSpy {
  std::vector<OptionTickerMsg> seen;
  template <class Ctx>
  void on_option_ticker(Ctx&, InstrumentId, const OptionTickerMsg& m) noexcept {
    seen.push_back(m);
  }
};
struct NoHooks {};

InstrumentTable make_table() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTC-15SEP26-77000-C";
  i.asset_class = AssetClass::Option;
  i.option_type = OptionType::Call;
  i.flags = Instrument::kEnabled | Instrument::kInverse;
  i.tick = Price::from_decimal("0.0001").value();
  i.lot = Qty::from_decimal("0.1").value();
  REQUIRE(t.add(i));
  return t;
}

OptionTickerMsg sample() {
  OptionTickerMsg m{};
  init_header(m, EventType::OptionTicker, InstrumentId{0}, VenueId{0});
  m.mark_price = Price::from_decimal("0.0069").value();
  m.underlying_price = Price::from_decimal("76904.4").value();
  m.index_price = Price::from_decimal("76900.24").value();
  m.mark_iv = 0.312;
  m.bid_iv = 0.2957;
  m.ask_iv = 0.3374;
  m.delta = 0.47736;
  m.gamma = 0.00028;
  m.vega = 18.43602;
  m.theta = -217.49347;
  m.rho = 1.31068;
  m.interest_rate = 0.0;
  m.hdr.exch_ts = Timestamp{1789344931096LL * 1'000'000};
  return m;
}

template <class S>
void run(S& strategy, MsgRing& journal) {
  const InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(1000).ns}};
  NullTransport transport;
  InlineFeed feed{1 << 16};
  EngineConfig cfg;
  auto engine = std::make_unique<Engine<S, SimClock, NullTransport, InlineFeed>>(
      cfg, table, clock, transport, feed, strategy, &journal);
  engine->start();
  const OptionTickerMsg m = sample();
  std::byte* p = feed.reserve(m.hdr.len);
  REQUIRE(p != nullptr);
  std::memcpy(p, &m, m.hdr.len);
  feed.commit();
  CHECK(engine->step() >= 1);
  CHECK(engine->stats().events == 1);
}

}  // namespace

TEST_CASE("core.engine: OptionTicker reaches on_option_ticker and the journal") {
  CHECK(to_string(EventType::OptionTicker) == "OptionTicker");
  CHECK(static_cast<int>(EventType::OptionTicker) == 24);
  CHECK(sizeof(OptionTickerMsg) == 192);

  TickerSpy spy;
  MsgRing journal(1 << 16);
  run(spy, journal);
  REQUIRE(spy.seen.size() == 1);
  const OptionTickerMsg expected = sample();
  CHECK(spy.seen[0].mark_iv == doctest::Approx(0.312));
  CHECK(spy.seen[0].underlying_price == expected.underlying_price);
  CHECK(spy.seen[0].vega == doctest::Approx(18.43602));

  bool journaled = false;
  while (const std::byte* p = journal.try_peek()) {
    const auto* h = reinterpret_cast<const EventHeader*>(p);
    if (h->type == EventType::OptionTicker) {
      journaled = true;
      CHECK(h->len == sizeof(OptionTickerMsg));
      const auto& j = msg_cast<OptionTickerMsg>(h);
      CHECK(j.mark_price == expected.mark_price);
      CHECK(j.theta == doctest::Approx(-217.49347));
      CHECK(std::memcmp(reinterpret_cast<const std::byte*>(&j) + sizeof(EventHeader),
                        reinterpret_cast<const std::byte*>(&expected) + sizeof(EventHeader),
                        sizeof(OptionTickerMsg) - sizeof(EventHeader)) == 0);
    }
    journal.release();
  }
  CHECK(journaled);

  NoHooks none;  // a strategy without the hook: the event is still consumed
  MsgRing journal2(1 << 16);
  run(none, journal2);
}
