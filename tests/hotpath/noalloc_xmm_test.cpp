// Xmm's hot path must not allocate (5.2): book updates on both instruments (basis EWMA, requote),
// maker fills that send a hedge, and the hedge's end.
#include "alloc_counter.hpp"
#include "test_support.hpp"

#include "fastmm/strategies/xmm.hpp"

#include <memory>

using namespace fastmm;
using fastmm::test::NoAllocScope;

namespace {

struct Book {
  Price bid{};
  Price ask{};
  Timestamp t{};
  [[nodiscard]] bool is_valid() const { return true; }
  [[nodiscard]] Price mid() const { return Price::from_raw((bid.raw + ask.raw) / 2); }
  [[nodiscard]] Level best_bid() const { return Level{bid, Qty::from_int(1)}; }
  [[nodiscard]] Level best_ask() const { return Level{ask, Qty::from_int(1)}; }
  [[nodiscard]] Timestamp last_update() const { return t; }
};
struct Pos {
  Qty qty{};
};
struct Ctx {
  const InstrumentTable* table = nullptr;
  Book books[2];
  Qty pos[2] = {};
  Qty open{};
  Timestamp t{1'789'344'931'096LL * 1'000'000};
  std::uint64_t quotes = 0;
  std::uint64_t sends = 0;
  [[nodiscard]] const InstrumentTable& instruments() const { return *table; }
  [[nodiscard]] const Instrument& instrument(InstrumentId id) const { return table->get(id); }
  [[nodiscard]] const Book& book(InstrumentId id) const { return books[id.value]; }
  [[nodiscard]] Pos position(InstrumentId id) const { return Pos{pos[id.value]}; }
  [[nodiscard]] Timestamp now() const { return t; }
  bool set_quotes(InstrumentId, const DesiredQuotes&) noexcept {
    ++quotes;
    return true;
  }
  void pull_quotes(InstrumentId) noexcept {}
  TimerId every(Duration, std::uint64_t) { return TimerId{1}; }
  Result<ClientOrderId, RejectReason> send(const NewOrderRequest& r) noexcept {
    ++sends;
    open = r.qty;
    return ClientOrderId{sends};
  }
  [[nodiscard]] Qty open_qty(InstrumentId id, Side) const { return id.value == 1 ? open : Qty{}; }
};

Instrument linear(const char* sym, std::uint8_t venue, const char* mult) {
  Instrument i{};
  i.symbol = sym;
  i.venue = VenueId{venue};
  i.asset_class = AssetClass::Perpetual;
  i.flags = Instrument::kEnabled;
  i.tick = Price::from_decimal("0.1").value();
  i.lot = Qty::from_decimal("0.001").value();
  i.min_qty = i.lot;
  i.contract_multiplier = Qty::from_decimal(mult).value();
  return i;
}

}  // namespace

TEST_CASE("hotpath.noalloc: Xmm requotes, hedges and ends hedges") {
  auto table = std::make_unique<InstrumentTable>();
  REQUIRE(table->add(linear("BTCUSDT", 0, "1")));
  REQUIRE(table->add(linear("BTCUSDT", 1, "1")));
  auto s = std::make_unique<Xmm>();
  REQUIRE_FALSE(s->configure({{"quote_qty", "0.01"},
                              {"requote_threshold_ticks", "0"},
                              {"basis_halflife_s", "30"},
                              {"hedge_retry_ms", "0"},
                              {"max_unhedged", "0.05"}}));
  Ctx ctx;
  ctx.table = table.get();
  s->on_start(ctx);
  REQUIRE(s->ready());
  const Qty lot = Qty::from_decimal("0.01").value();
  {
    NoAllocScope guard(true);
    for (int i = 0; i < 2000; ++i) {
      ctx.t = ctx.t + milliseconds(1);
      const Price mid = Price::from_int(100000 + (i % 50));
      ctx.books[1] = Book{mid - Price::from_decimal("0.1").value(), mid, ctx.t};
      ctx.books[0] = Book{mid - Price::from_int(10), mid + Price::from_int(10), ctx.t};
      s->on_book(ctx, InstrumentId{1}, ctx.books[1]);
      s->on_book(ctx, InstrumentId{0}, ctx.books[0]);
      // A maker fill and the hedge that follows it.
      ctx.pos[0] += lot;
      Fill f;
      f.instrument = InstrumentId{0};
      f.qty = lot;
      s->on_fill(ctx, f);
      ctx.pos[1] -= ctx.open;
      ctx.open = Qty{};
      OmsUpdate u;
      u.terminal = true;
      u.changed = true;
      u.order.instrument = InstrumentId{1};
      u.order.state = OrderState::Filled;
      u.order.cum_qty = lot;
      s->on_order_update(ctx, u);
      s->on_timer(ctx, TimerId{1}, Xmm::kTimer);
    }
  }
  CHECK(ctx.sends == 2000);
  CHECK(ctx.quotes >= 2000);
}
