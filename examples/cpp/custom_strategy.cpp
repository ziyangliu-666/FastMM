// A custom strategy in ~60 lines, backtested on the synthetic market (README highlight).
//
// MicropriceMM quotes one level each side around the size-weighted microprice
//   micro = (bid * ask_qty + ask * bid_qty) / (bid_qty + ask_qty)
// `edge_ticks` away, and stops quoting the side that would push |position| past the limit.
// Parameters are exact (Qty is parsed from the config string, never through a double), and the
// hot path is integer-only. No registration is needed for run_backtest<S>();
// examples/external-project/ registers the same strategy for the live, backtest and replay command
// lines (docs/how-to/strategies/register-a-strategy.md). A hook with a wrong signature is a compile
// error.
#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/strategy.hpp"

#include <cstdio>

using namespace fastmm;

struct MicropriceParams {
  FASTMM_PARAMS(MicropriceParams)
  FASTMM_PARAM(int, edge_ticks, 2, 0, 1000, "distance from the microprice, ticks")
  FASTMM_PARAM(Qty, quote_qty, 0.002_qty, 0_qty, 1000_qty, "quantity per side")
  FASTMM_PARAM(Qty, max_position, 0.004_qty, 0_qty, 1000_qty, "inventory limit (0 = none)")

  // Cross-field check, run once all parameters are applied.
  [[nodiscard]] std::optional<std::string> validate() const {
    if (max_position.is_positive() && quote_qty > max_position)
      return "quote_qty must not exceed max_position";
    return std::nullopt;
  }
};

class MicropriceMM : public StrategyBase<MicropriceParams> {
 public:
  static constexpr std::string_view name() noexcept { return "microprice_mm"; }

  void on_book(auto& ctx, InstrumentId id, const auto& book) noexcept {
    if (!book.is_valid()) return ctx.pull_quotes(id);
    const MicropriceParams& p = params();
    const Instrument& inst = ctx.instrument(id);
    const Price micro = microprice(book.best_bid(), book.best_ask());
    const Price edge = inst.ticks(p.edge_ticks);
    const Qty qty = inst.round_qty(p.quote_qty);
    const Qty pos = ctx.position(id).qty;

    DesiredQuotes q;
    if (inventory_allows(Side::Buy, pos, qty, p.max_position))
      q.bid(inst.round_price(micro - edge, Side::Buy), qty);
    if (inventory_allows(Side::Sell, pos, qty, p.max_position))
      q.ask(inst.round_price(micro + edge, Side::Sell), qty);
    ctx.set_quotes(id, q);  // QuoteManager diffs against resting orders
  }
};
static_assert(verify_strategy<MicropriceMM>());

int main() {
  auto cfg = bt::BacktestConfig::single_instrument("BTCUSDT", 0.01_px, 0.00001_qty);
  cfg.duration = seconds(60);
  cfg.set_seed(7);
  cfg.generator.limit_rate_per_s = 400;  // a busier synthetic market than the defaults
  cfg.generator.market_rate_per_s = 30;
  cfg.generator.market_qty_median_lots = 1500;
  cfg.generator.mid_step_rate_per_s = 20;
  cfg.transport.fees = sim::FeeModel::from_bps(10.0, 10.0);  // Binance spot VIP 0: 0.1 % both sides
  cfg.params = {{"edge_ticks", "1"}};
  cfg.measure_wall_clock = false;

  const bt::BacktestResult r = bt::run_backtest<MicropriceMM>(cfg);  // synthetic market
  std::fputs(r.summary_table().c_str(), stdout);
  return r.metrics.fills > 0 ? 0 : 1;
}
