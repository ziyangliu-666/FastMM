// A quoter whose fills are hedged on other venues by HedgeExecutor (docs/how-to/strategies/
// hedge-executor.md), run through the real engine and a simulated venue.
//
// HedgedMM quotes one level each side of instrument 0's mid, half_spread_bps away. Its position
// there is the executor's source; instruments 1 and 2 (other venues) are the hedges, in that
// order. The quotes come off while no hedge venue can take a hedge, and only the side that reduces
// the exposure is quoted while a hedge is held for lack of balance. When no hedge venue has taken
// the exposure for derisk_after_ms, the executor works it off on instrument 0 itself.
//
// main() quotes on A, fills a bid, and watches the hedge go to B; then B's kill switch trips and
// the next fill is hedged on C; then both are down and the exposure is de-risked on A.
#include "fastmm/strategy.hpp"
#include "fastmm/testing/strategy_harness.hpp"

#include <cstdio>

using namespace fastmm;

struct HedgedParams {
  FASTMM_PARAMS(HedgedParams)
  FASTMM_PARAM_BPS(half_spread_bps, 5_bps, 0_bps, 10000_bps, "distance from the mid")
  FASTMM_PARAM(Qty, quote_qty, 0.01_qty, 0_qty, 1000_qty, "quantity per side, base units")
  FASTMM_PARAM_BPS(hedge_tolerance_bps, 5_bps, 0_bps, 1000_bps, "hedge IOC: through the touch")
  FASTMM_PARAM_MS(derisk_after_ms,
                  milliseconds(2000),
                  milliseconds(0),
                  milliseconds(3600000),
                  "de-risk after this long without a hedge venue (0 = never)")
};

class HedgedMM : public StrategyBase<HedgedParams> {
 public:
  static constexpr std::string_view name() noexcept { return "hedged_mm"; }
  static constexpr std::uint64_t kTimer = 1;

  // [start:start]
  void on_start(auto& ctx) noexcept {
    const HedgedParams& p = params();
    HedgeExecutor::Config c;
    c.name = "hedged_mm";
    c.tag = 0x4844'4745;  // any tag outside the quote manager's range
    c.derisk_tag = 0x4844'5253;
    c.stale = milliseconds(2000);
    c.derisk_after = p.derisk_after_ms;
    c.derisk_step = p.quote_qty;
    c.derisk_interval = milliseconds(500);
    c.derisk_tolerance = p.hedge_tolerance_bps;
    hedge_.reset();
    static_cast<void>(hedge_.add_source(kQuote));
    static_cast<void>(hedge_.add_hedge(InstrumentId{1}, p.hedge_tolerance_bps));
    static_cast<void>(hedge_.add_hedge(InstrumentId{2}, p.hedge_tolerance_bps));
    if (!hedge_.start(ctx, c)) return;
    static_cast<void>(ctx.every(milliseconds(100), kTimer));
  }
  // [end:start]

  // [start:hooks]
  // The executor sees every hook it needs; the quotes follow what it can do.
  void on_book(auto& ctx, InstrumentId id, const auto&) noexcept {
    hedge_.on_book(ctx, id);
    quote(ctx);
  }
  void on_fill(auto& ctx, const Fill& f) noexcept {
    if (hedge_.on_fill(ctx, f)) quote(ctx);
  }
  void on_order_update(auto& ctx, const OmsUpdate& u) noexcept {
    if (hedge_.on_order_update(ctx, u)) quote(ctx);
  }
  void on_timer(auto& ctx, TimerId, std::uint64_t) noexcept {
    hedge_.on_timer(ctx);
    quote(ctx);
  }
  void on_connection(auto& ctx, const ConnectionStateMsg& m) noexcept {
    hedge_.on_connection(ctx, m);
    quote(ctx);
  }
  void on_balance(auto& ctx, const BalanceMsg& m) noexcept {
    if (hedge_.on_balance(ctx, m)) quote(ctx);
  }
  // [end:hooks]

  [[nodiscard]] const HedgeExecutor& hedger() const noexcept { return hedge_; }

 private:
  static constexpr InstrumentId kQuote{0};

  // [start:quote]
  void quote(auto& ctx) noexcept {
    const auto& b = ctx.book(kQuote);
    if (!hedge_.ready() || !b.is_valid() || !hedge_.can_hedge(ctx)) {
      ctx.pull_quotes(kQuote);
      return;
    }
    const Instrument& inst = ctx.instrument(kQuote);
    const Price m = b.mid();
    const Price half = m * params().half_spread_bps;
    const Qty qty = inst.round_qty(params().quote_qty);
    const Qty open = hedge_.residual(ctx);
    DesiredQuotes q;
    if (!(hedge_.held() && open.is_positive())) q.bid(inst.round_price(m - half, Side::Buy), qty);
    if (!(hedge_.held() && open.is_negative())) q.ask(inst.round_price(m + half, Side::Sell), qty);
    keep_passive(q, b.best_bid().price, b.best_ask().price, inst.tick);
    ctx.set_quotes(kQuote, q);
  }
  // [end:quote]

  HedgeExecutor hedge_;
};
static_assert(verify_strategy<HedgedMM>());

// ---- a session on the simulated venues
// ------------------------------------------------------------

namespace {

using sim::StrategyHarness;

Instrument perp(const char* symbol, std::uint8_t venue) {
  Instrument i{};
  i.symbol = symbol;
  i.venue = VenueId{venue};
  i.asset_class = AssetClass::Perpetual;
  i.flags = Instrument::kEnabled;
  i.tick = 0.1_px;
  i.lot = 0.001_qty;
  i.min_qty = i.lot;
  i.contract_multiplier = 1_qty;
  return i;
}

// A buyer resting at `price` on instrument `id`, for the hedges and the de-risk order to sell into.
void bid_at(StrategyHarness<HedgedMM>& h, std::uint32_t id, Price price) {
  static std::uint64_t seq = 0;
  sim::NewOrder o;
  o.account = 9;
  o.cl_ord_id = ClientOrderId{++seq};
  o.instrument = InstrumentId{id};
  o.side = Side::Buy;
  o.price = price;
  o.qty = 1_qty;
  static_cast<void>(h.venue_transport().matching_engine().submit(o, h.now()));
}

void kill_switch(StrategyHarness<HedgedMM>& h, ControlCommand cmd, std::uint8_t venue) {
  ControlMsg m{};
  init_header(m, EventType::Control, InstrumentId{}, VenueId{venue});
  m.command = cmd;
  m.arg = static_cast<std::uint64_t>(KillReason::VenueFatal);
  h.push(m.hdr);
}

void print(const char* when, StrategyHarness<HedgedMM>& h) {
  auto& e = h.engine();
  const HedgeExecutor& x = h.strategy().hedger();
  std::printf("%-36s A %8.3f  B %8.3f  C %8.3f  %-11s sent %llu failovers %llu de-risk %llu\n",
              when,
              e.position(InstrumentId{0}).qty.to_double(),
              e.position(InstrumentId{1}).qty.to_double(),
              e.position(InstrumentId{2}).qty.to_double(),
              HedgeExecutor::to_string(x.state()).data(),
              static_cast<unsigned long long>(x.stats().hedges_sent),
              static_cast<unsigned long long>(x.stats().failovers),
              static_cast<unsigned long long>(x.stats().derisk_orders));
}

}  // namespace

int main() {
  sim::HarnessOptions o;
  o.instruments.clear();
  static_cast<void>(o.instruments.add(perp("BTCUSDT", 0)));  // A: quotes
  static_cast<void>(o.instruments.add(perp("BTCUSDT", 1)));  // B: hedges
  static_cast<void>(o.instruments.add(perp("BTCUSDT", 2)));  // C: fallback
  StrategyHarness<HedgedMM> h({}, o);
  for (std::uint32_t i = 0; i < 3; ++i) h.book(100000.0_px, 100000.2_px, 1_qty, InstrumentId{i});
  bid_at(h, 1, 100000.0_px);
  bid_at(h, 2, 100000.0_px);
  h.advance(milliseconds(1));

  h.fill(Side::Buy);  // our bid on A: long 0.01
  h.advance(milliseconds(5));
  print("A bid filled, hedged on B", h);

  kill_switch(h, ControlCommand::TripVenueKill, 1);
  h.fill(Side::Buy);
  h.advance(milliseconds(5));
  print("B killed, the next fill hedged on C", h);

  kill_switch(h, ControlCommand::TripVenueKill, 2);
  h.fill(Side::Buy);  // lands as C goes: no hedge venue left, the quotes come off
  h.advance(milliseconds(5));
  print("C killed too, a fill unhedged", h);
  bid_at(h, 0, 100000.0_px);
  h.advance(seconds(3));  // derisk_after_ms: the executor sells it back on A
  print("3 s later: de-risked on A", h);

  kill_switch(h, ControlCommand::ResetKill, 1);
  // The books have not moved for 3 s: stale, so B cannot hedge until they update.
  for (std::uint32_t i = 0; i < 3; ++i) h.book(100000.0_px, 100000.2_px, 1_qty, InstrumentId{i});
  h.advance(milliseconds(200));
  print("B back: quoting again", h);

  const HedgeExecutor& x = h.strategy().hedger();
  const bool ok = x.stats().hedges_sent == 2 && x.stats().failovers == 1 &&
                  x.stats().derisk_orders == 1 && x.residual(h.engine().context()).is_zero() &&
                  h.working_orders(InstrumentId{0}).size() == 2;
  return ok ? 0 : 1;
}
