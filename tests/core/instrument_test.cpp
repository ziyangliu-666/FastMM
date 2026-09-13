#include "fastmm/core/instrument.hpp"

#include "test_support.hpp"

using namespace fastmm;

namespace {
Instrument make(const char* sym, VenueId venue = VenueId{0}) {
  Instrument i{};
  i.symbol = sym;
  i.venue = venue;
  i.asset_class = AssetClass::Spot;
  i.flags = Instrument::kEnabled;
  i.tick = Price::from_decimal("0.01").value();
  i.lot = Qty::from_decimal("0.001").value();
  i.min_qty = Qty::from_decimal("0.001").value();
  i.max_qty = Qty::from_int(1000);
  i.min_notional = Notional::from_int(5);
  return i;
}
}  // namespace

TEST_CASE("core.instrument: layout and helpers") {
  static_assert(sizeof(Instrument) == 128);
  Instrument i = make("BTCUSDT");
  CHECK(i.enabled());
  CHECK(i.round_price(Price::from_decimal("100.129").value(), Side::Buy) ==
        Price::from_decimal("100.12").value());
  CHECK(i.round_price(Price::from_decimal("100.121").value(), Side::Sell) ==
        Price::from_decimal("100.13").value());
  CHECK(i.round_qty(Qty::from_decimal("1.23456").value()) == Qty::from_decimal("1.234").value());
  CHECK(i.valid_price(Price::from_decimal("100.12").value()));
  CHECK_FALSE(i.valid_price(Price::from_decimal("100.123").value()));
  CHECK_FALSE(i.valid_price(Price{}));
  CHECK(i.valid_qty(Qty::from_decimal("0.5").value()));
  CHECK_FALSE(i.valid_qty(Qty::from_decimal("0.0005").value()));  // below min & off lot
  CHECK_FALSE(i.valid_qty(Qty::from_int(1001)));
  CHECK(i.notional(Price::from_int(50000), Qty::from_decimal("0.1").value()) ==
        Notional::from_int(5000));
  CHECK(i.pnl_per_tick() == Notional::from_decimal("0.01").value());
  i.contract_multiplier = Qty::from_int(100);
  CHECK(i.notional(Price::from_int(10), Qty::from_int(2)) == Notional::from_int(2000));
  CHECK(i.pnl_per_tick() == Notional::from_int(1));
  CHECK_FALSE(i.is_derivative());
}

TEST_CASE("core.instrument_table: dense ids and symbol lookup") {
  InstrumentTable t;
  auto a = t.add(make("BTCUSDT"));
  auto b = t.add(make("ETHUSDT"));
  auto c = t.add(make("BTCUSDT", VenueId{1}));  // same symbol other venue OK
  REQUIRE(a);
  REQUIRE(b);
  REQUIRE(c);
  CHECK(a->value == 0);
  CHECK(b->value == 1);
  CHECK(c->value == 2);
  CHECK(t.size() == 3);
  CHECK(t.get(*b).symbol == "ETHUSDT");
  CHECK(t[*a].id == *a);
  const Instrument* f = t.find(VenueId{1}, "BTCUSDT");
  REQUIRE(f != nullptr);
  CHECK(f->id == *c);
  CHECK(t.find(VenueId{0}, "XRPUSDT") == nullptr);
  CHECK(t.add(make("BTCUSDT")).error() == InstrumentError::DuplicateSymbol);
  Instrument bad = make("");
  CHECK(t.add(bad).error() == InstrumentError::EmptySymbol);
  bad = make("X");
  bad.tick = Price{};
  CHECK(t.add(bad).error() == InstrumentError::InvalidTick);
  bad = make("Y");
  bad.lot = Qty{};
  CHECK(t.add(bad).error() == InstrumentError::InvalidLot);
  int n = 0;
  for (const auto& inst : t) {
    CHECK(inst.id.value == static_cast<std::uint32_t>(n));
    ++n;
  }
  CHECK(n == 3);
  for (int k = 0; k < 300; ++k) {
    auto r = t.add(make(("S" + std::to_string(k)).c_str()));
    if (t.size() == kMaxInstruments) {
      CHECK((r.has_value() || r.error() == InstrumentError::TableFull));
    }
  }
  CHECK(t.size() == kMaxInstruments);
}
