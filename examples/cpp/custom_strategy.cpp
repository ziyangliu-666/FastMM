// A custom strategy in ~60 lines, backtested on the synthetic market (README highlight).
//
// MicropriceMM quotes one level each side around the size-weighted microprice
//   micro = (bid * ask_qty + ask * bid_qty) / (bid_qty + ask_qty)
// `edge_ticks` away, and stops quoting the side that would push |position| past the limit.
// No registration is needed for run_backtest<S>(); add FASTMM_REGISTER_STRATEGY to make it
// visible to the apps and Python.
#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/strategies/strategy.hpp"

#include <cstdio>

using namespace fastmm;

struct MicropriceParams {
  FASTMM_PARAMS(MicropriceParams)
  FASTMM_PARAM(int, edge_ticks, 2, 0, 1000, "distance from the microprice, ticks")
  FASTMM_PARAM(double, quote_qty, 0.002, 0.0, 1e9, "quantity per side")
  FASTMM_PARAM(double, max_position, 0.004, 0.0, 1e9, "absolute inventory limit")
};

class MicropriceMM : public StrategyBase<MicropriceParams> {
 public:
  static constexpr std::string_view name() noexcept { return "microprice_mm"; }

  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book& book) noexcept {
    if (!book.is_valid()) return ctx.pull_quotes(id);
    const Instrument& inst = ctx.instrument(id);
    const Level bid = book.best_bid();
    const Level ask = book.best_ask();
    const Int128 w = static_cast<Int128>(bid.qty.raw) + ask.qty.raw;  // integer microprice
    const auto micro = Price::from_raw(
        static_cast<std::int64_t>((static_cast<Int128>(bid.price.raw) * ask.qty.raw +
                                   static_cast<Int128>(ask.price.raw) * bid.qty.raw) /
                                  w));
    const Price edge = inst.tick * params_.edge_ticks;
    const Qty qty = inst.round_qty(Qty::from_double(params_.quote_qty));
    const Qty limit = Qty::from_double(params_.max_position);
    const Qty pos = ctx.position(id).qty;

    DesiredQuotes q;
    if (pos + qty <= limit)
      static_cast<void>(q.bids.push_back({inst.round_price(micro - edge, Side::Buy), qty}));
    if (pos - qty >= -limit)
      static_cast<void>(q.asks.push_back({inst.round_price(micro + edge, Side::Sell), qty}));
    ctx.set_quotes(id, q);  // QuoteManager diffs against resting orders
  }
};

int main() {
  auto cfg = bt::BacktestConfig::single_instrument(
      "BTCUSDT", Price::from_decimal("0.01").value(), Qty::from_decimal("0.00001").value());
  cfg.duration = seconds(60);
  cfg.set_seed(7);
  cfg.generator.limit_rate_per_s = 400;  // a busier synthetic market than the defaults
  cfg.generator.market_rate_per_s = 30;
  cfg.generator.market_qty_median_lots = 1500;
  cfg.generator.mid_step_rate_per_s = 20;
  cfg.transport.fees = sim::FeeModel::from_bps(-0.5, 3.0);  // maker rebate, taker fee
  cfg.params = {{"edge_ticks", "1"}};
  cfg.measure_wall_clock = false;

  const bt::BacktestResult r = bt::run_backtest<MicropriceMM>(cfg);  // synthetic market
  std::fputs(r.summary_table().c_str(), stdout);
  return r.metrics.fills > 0 ? 0 : 1;
}
