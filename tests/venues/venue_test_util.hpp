#pragma once
// Shared helpers for the venue tests: fixture loading with simdjson padding, a symbol table
// for BTCUSDT/ETHUSDT on venue 0 (Binance) and venue 1 (Bybit), and a ring-backed sink that
// records every message pushed into it.
#include "test_support.hpp"

#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/padded_json.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace fastmm::venues::test {

inline PaddedJson padded_fixture(const std::string& rel) {
  return PaddedJson(fastmm::test::fixture(rel));
}

inline Instrument make_instrument(const char* symbol,
                                  std::uint8_t venue,
                                  const char* base,
                                  const char* quote) {
  Instrument i{};
  i.symbol = symbol;
  i.venue = VenueId{venue};
  i.base = base;
  i.quote = quote;
  i.asset_class = AssetClass::Spot;
  i.flags = Instrument::kEnabled;
  i.tick = Price::from_decimal("0.01").value();
  i.lot = Qty::from_decimal("0.00001").value();
  i.min_qty = i.lot;
  i.min_notional = Notional::from_int(5);
  return i;
}

struct TestUniverse {
  InstrumentTable instruments;
  SymbolTable symbols;
  TestUniverse() {
    REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));  // id 0
    REQUIRE(instruments.add(make_instrument("ETHUSDT", 0, "ETH", "USDT")));  // id 1
    REQUIRE(instruments.add(make_instrument("BTCUSDT", 1, "BTC", "USDT")));  // id 2 (bybit)
    REQUIRE(symbols.build(instruments));
  }
};

// Copies every committed message out of a MsgRing-backed sink.
struct RecordingSink {
  MsgRing ring;
  EventSink sink;
  explicit RecordingSink(std::size_t bytes = 1U << 20, SinkPolicy policy = SinkPolicy::Drop)
      : ring(bytes), sink(&ring, policy, 10) {}

  std::vector<std::vector<std::byte>> drain() {
    std::vector<std::vector<std::byte>> out;
    while (const std::byte* p = ring.try_peek()) {
      const auto* h = reinterpret_cast<const EventHeader*>(p);
      out.emplace_back(p, p + h->len);
      ring.release();
    }
    return out;
  }
  template <class M>
  static const M& as(const std::vector<std::byte>& raw) {
    return *reinterpret_cast<const M*>(raw.data());
  }
  static EventType type_of(const std::vector<std::byte>& raw) {
    return reinterpret_cast<const EventHeader*>(raw.data())->type;
  }
};

struct Scratch {
  alignas(64) std::byte buf[kDecoderScratchBytes];
  std::span<std::byte> span() noexcept { return {buf, sizeof buf}; }
  template <class M>
  const M& as() const noexcept {
    return *reinterpret_cast<const M*>(buf);
  }
};

}  // namespace fastmm::venues::test
