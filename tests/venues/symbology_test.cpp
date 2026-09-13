#include "fastmm/venues/symbology.hpp"

#include "test_support.hpp"

using namespace fastmm;
using namespace fastmm::venues;

namespace {
Instrument make(const char* sym, std::uint8_t venue) {
  Instrument i{};
  i.symbol = sym;
  i.venue = VenueId{venue};
  i.tick = Price::from_decimal("0.01").value();
  i.lot = Qty::from_decimal("0.001").value();
  return i;
}
}  // namespace

TEST_CASE("venues.symbology: case-insensitive lookup keyed by venue") {
  InstrumentTable t;
  REQUIRE(t.add(make("BTCUSDT", 0)));
  REQUIRE(t.add(make("ETHUSDT", 0)));
  REQUIRE(t.add(make("BTCUSDT", 1)));
  SymbolTable s;
  REQUIRE(s.build(t));
  CHECK(s.size() == 3);
  CHECK(s.find(VenueId{0}, "BTCUSDT") == InstrumentId{0});
  CHECK(s.find(VenueId{0}, "btcusdt") == InstrumentId{0});
  CHECK(s.find(VenueId{0}, "ethusdt") == InstrumentId{1});
  CHECK(s.find(VenueId{1}, "BtcUsdt") == InstrumentId{2});
  CHECK_FALSE(s.find(VenueId{2}, "BTCUSDT").valid());
  CHECK_FALSE(s.find(VenueId{0}, "BTCUSD").valid());
  CHECK_FALSE(s.find(VenueId{0}, "").valid());
  CHECK(s.venue_symbol(InstrumentId{2}) == "BTCUSDT");
  CHECK(s.lower_symbol(InstrumentId{1}) == "ethusdt");
  CHECK(s.venue_of(InstrumentId{2}) == VenueId{1});
  CHECK(s.venue_symbol(InstrumentId{99}).empty());
  CHECK_FALSE(s.contains(InstrumentId{3}));
}

TEST_CASE("venues.symbology: rejects duplicates and empties, survives collisions") {
  SymbolTable s;
  CHECK(s.add(InstrumentId{0}, VenueId{0}, "AAA"));
  CHECK_FALSE(s.add(InstrumentId{1}, VenueId{0}, "aaa"));  // same key, different case
  CHECK(s.add(InstrumentId{1}, VenueId{1}, "aaa"));
  CHECK_FALSE(s.add(InstrumentId{2}, VenueId{0}, ""));
  // Fill many entries to force probe chains.
  for (std::uint32_t i = 2; i < 200; ++i) {
    char name[16];
    std::snprintf(name, sizeof name, "S%u", i);
    REQUIRE(s.add(InstrumentId{i}, VenueId{0}, name));
  }
  for (std::uint32_t i = 2; i < 200; ++i) {
    char name[16];
    std::snprintf(name, sizeof name, "s%u", i);
    CHECK(s.find(VenueId{0}, name) == InstrumentId{i});
  }
  CHECK(symbol_hash("btcusdt") == symbol_hash("BTCUSDT"));
  CHECK(symbol_hash("btcusdt") != symbol_hash("BTCUSDC"));
}
