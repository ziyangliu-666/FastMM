// Options hot path: Black-76 greeks, the implied-volatility solver and OptionsMM's ticker -> quote
// path must not allocate (5.2).
#include "alloc_counter.hpp"
#include "test_support.hpp"

#include "fastmm/core/options/black76.hpp"
#include "fastmm/strategies/options_mm.hpp"

#include <memory>

using namespace fastmm;
using fastmm::test::NoAllocScope;

namespace {

struct Book {
  [[nodiscard]] bool is_valid() const { return true; }
  [[nodiscard]] Price mid() const { return Price{}; }
  [[nodiscard]] Level best_bid() const {
    return Level{Price::from_decimal("0.0001").value(), Qty{}};
  }
  [[nodiscard]] Level best_ask() const { return Level{Price::from_decimal("0.9").value(), Qty{}}; }
};
struct Pos {
  Qty qty{};
};
struct Ctx {
  const InstrumentTable* table = nullptr;
  Book b;
  Timestamp t{1'789'344'931'096LL * 1'000'000};
  std::uint64_t quotes = 0;
  [[nodiscard]] const InstrumentTable& instruments() const { return *table; }
  [[nodiscard]] const Instrument& instrument(InstrumentId id) const { return table->get(id); }
  [[nodiscard]] const Book& book(InstrumentId) const { return b; }
  [[nodiscard]] Pos position(InstrumentId) const { return Pos{Qty::from_decimal("0.3").value()}; }
  [[nodiscard]] Timestamp now() const { return t; }
  bool set_quotes(InstrumentId, const DesiredQuotes&) noexcept {
    ++quotes;
    return true;
  }
  void pull_quotes(InstrumentId) noexcept {}
  TimerId every(Duration, std::uint64_t) { return TimerId{1}; }
};

}  // namespace

TEST_CASE("hotpath.noalloc: Black-76, implied vol and OptionsMM requotes") {
  auto table = std::make_unique<InstrumentTable>();
  Instrument call{};
  call.symbol = "BTC-C";
  call.asset_class = AssetClass::Option;
  call.option_type = OptionType::Call;
  call.strike = Price::from_int(77000);
  call.expiry_ns = 1'789'344'931'096LL * 1'000'000 + 30LL * 86'400 * 1'000'000'000;
  call.flags = Instrument::kEnabled | Instrument::kInverse;
  call.tick = Price::from_decimal("0.0001").value();
  call.lot = Qty::from_decimal("0.1").value();
  REQUIRE(table->add(call));
  auto strategy = std::make_unique<OptionsMM>();
  REQUIRE_FALSE(strategy->configure({{"quote_qty", "0.1"}, {"requote_threshold_ticks", "0"}}));
  Ctx ctx;
  ctx.table = table.get();
  strategy->on_start(ctx);
  OptionTickerMsg m{};
  init_header(m, EventType::OptionTicker, InstrumentId{0}, VenueId{0});
  m.underlying_price = Price::from_decimal("76904.4").value();
  m.mark_iv = 0.5;
  double sum = 0.0;
  {
    NoAllocScope guard(true);
    for (int i = 0; i < 2000; ++i) {
      const double f = 70000.0 + i;
      const options::Greeks g = options::black76(options::CallPut::Call, f, 77000.0, 0.08, 0.5);
      const options::IvResult iv =
          options::implied_vol(options::CallPut::Call, g.price, f, 77000.0, 0.08, 0.0, 1e-9);
      sum += g.delta + iv.vol;
      m.underlying_price = Price::from_double(f);
      strategy->on_option_ticker(ctx, m.hdr.instrument, m);
    }
  }
  CHECK(sum > 0.0);
  CHECK(ctx.quotes == 2000);
}
