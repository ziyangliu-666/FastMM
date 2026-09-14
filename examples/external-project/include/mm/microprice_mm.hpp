#pragma once
// MicropriceMM quotes one level each side around the size-weighted microprice
//   micro = (bid * ask_qty + ask * bid_qty) / (bid_qty + ask_qty)
// `edge_ticks` away, and stops quoting the side that would push |position| past the limit.
// Parameters are exact (Qty is parsed from the config string, never through a double), and the
// hot path is integer-only. A hook with a wrong signature is a compile error.
#include "fastmm/strategy.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace mm {

using namespace fastmm::literals;

struct MicropriceParams {
  FASTMM_PARAMS(MicropriceParams)
  FASTMM_PARAM(int, edge_ticks, 2, 0, 1000, "distance from the microprice, ticks")
  FASTMM_PARAM(fastmm::Qty, quote_qty, 0.002_qty, 0_qty, 1000_qty, "quantity per side")
  FASTMM_PARAM(fastmm::Qty, max_position, 0.004_qty, 0_qty, 1000_qty, "inventory limit (0 = none)")

  // Cross-field check, run once all parameters are applied.
  [[nodiscard]] std::optional<std::string> validate() const {
    if (max_position.is_positive() && quote_qty > max_position)
      return "quote_qty must not exceed max_position";
    return std::nullopt;
  }
};

class MicropriceMM : public fastmm::StrategyBase<MicropriceParams> {
 public:
  static constexpr std::string_view name() noexcept { return "microprice_mm"; }

  void on_book(auto& ctx, fastmm::InstrumentId id, const auto& book) noexcept {
    if (!book.is_valid()) return ctx.pull_quotes(id);
    const MicropriceParams& p = params();
    const fastmm::Instrument& inst = ctx.instrument(id);
    const fastmm::Price micro = fastmm::microprice(book.best_bid(), book.best_ask());
    const fastmm::Price edge = inst.ticks(p.edge_ticks);
    const fastmm::Qty qty = inst.round_qty(p.quote_qty);
    const fastmm::Qty pos = ctx.position(id).qty;

    fastmm::DesiredQuotes q;
    if (fastmm::inventory_allows(fastmm::Side::Buy, pos, qty, p.max_position))
      q.bid(inst.round_price(micro - edge, fastmm::Side::Buy), qty);
    if (fastmm::inventory_allows(fastmm::Side::Sell, pos, qty, p.max_position))
      q.ask(inst.round_price(micro + edge, fastmm::Side::Sell), qty);
    ctx.set_quotes(id, q);  // the QuoteManager diffs against resting orders
  }

  // Quoting paused or resumed (operator pull, kill switch, reconciliation): requote as soon as it
  // is back, even if the book has not changed.
  void on_quoting(auto& ctx, bool enabled) noexcept {
    if (!enabled) return;
    for (const fastmm::Instrument& inst : ctx.instruments())
      on_book(ctx, inst.id, ctx.book(inst.id));
  }
};

static_assert(fastmm::verify_strategy<MicropriceMM>());

}  // namespace mm
