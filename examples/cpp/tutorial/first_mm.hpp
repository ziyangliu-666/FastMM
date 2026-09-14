#pragma once
// FirstMM, the strategy of the tutorial "Your first market maker"
// (docs/tutorials/first-strategy/). It quotes one level on each side of the size-weighted
// microprice, `edge_bps` away, stops adding to a position that has reached `max_position`, and
// logs what happens to it: fills, lost connections and paused quoting.
#include "fastmm/strategy.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tutorial {

using namespace fastmm;

// [start:params]
struct FirstMMParams {
  FASTMM_PARAMS(FirstMMParams)
  FASTMM_PARAM_BPS(
      edge_bps, 5_bps, 0_bps, 500_bps, "distance of each quote from the microprice, bps")
  FASTMM_PARAM(Qty, quote_qty, 0.001_qty, 0_qty, 1000_qty, "quantity per side, base units")
  FASTMM_PARAM(
      Qty, max_position, 0.004_qty, 0_qty, 1000_qty, "position limit, base units (0 = none)")
  FASTMM_PARAM_MS(report_ms,
                  milliseconds(10000),
                  milliseconds(0),
                  milliseconds(3600000),
                  "interval of the status log line, ms (0 = off)")

  // Checked after all parameters are applied: a quote larger than the limit could never be sent.
  [[nodiscard]] std::optional<std::string> validate() const {
    if (max_position.is_positive() && quote_qty > max_position)
      return "quote_qty must not exceed max_position";
    return std::nullopt;
  }
};
// [end:params]

// [start:compute_quotes]
// The quotes for one book: integer arithmetic on plain values, no engine state.
[[nodiscard]] inline DesiredQuotes compute_quotes(const FirstMMParams& p,
                                                  const Instrument& inst,
                                                  Level best_bid,
                                                  Level best_ask,
                                                  Qty position) noexcept {
  const Price micro = microprice(best_bid, best_ask);
  const Price edge = micro * p.edge_bps;  // Price * Ratio: exact, truncated toward zero
  const Qty qty = inst.round_qty(p.quote_qty);
  DesiredQuotes q;
  if (inventory_allows(Side::Buy, position, qty, p.max_position))
    q.bid(inst.round_price(micro - edge, Side::Buy), qty);  // rounds down to the tick
  if (inventory_allows(Side::Sell, position, qty, p.max_position))
    q.ask(inst.round_price(micro + edge, Side::Sell), qty);    // rounds up to the tick
  keep_passive(q, best_bid.price, best_ask.price, inst.tick);  // post-only quotes must not cross
  return q;
}
// [end:compute_quotes]

class FirstMM : public StrategyBase<FirstMMParams> {
 public:
  static constexpr std::string_view name() noexcept { return "first_mm"; }

  // [start:on_start]
  void on_start(auto& ctx) noexcept {
    if (params().report_ms > Duration{}) report_timer_ = ctx.every(params().report_ms);
    FASTMM_LOG_INFO("first_mm: started, quoting {}",
                    ctx.quoting_enabled() ? "enabled" : "disabled (dry run)");
  }
  // [end:on_start]

  // [start:on_book]
  void on_book(auto& ctx, InstrumentId id, const auto& book) noexcept {
    if (!book.is_valid()) return ctx.pull_quotes(id);  // empty or crossed
    const DesiredQuotes q = compute_quotes(
        params(), ctx.instrument(id), book.best_bid(), book.best_ask(), ctx.position(id).qty);
    ctx.set_quotes(id, q);  // the engine sends only the difference to the resting orders
  }
  // [end:on_book]

  // [start:on_fill]
  void on_fill(auto& /*ctx*/, const Fill& fill) noexcept {
    ++fills_;
    if (fill.late) ++late_fills_;  // the order had already been cancelled
  }
  // [end:on_fill]

  // [start:on_connection]
  void on_connection(auto& /*ctx*/, const ConnectionStateMsg& m) noexcept {
    if (m.state == ConnState::Live) {
      FASTMM_LOG_INFO("first_mm: venue {} channel {} is live", m.hdr.venue.value, m.channel);
      return;
    }
    ++disconnects_;  // the engine has already pulled this venue's quotes
    FASTMM_LOG_WARN("first_mm: venue {} channel {} is {}; quotes pulled",
                    m.hdr.venue.value,
                    m.channel,
                    to_string(m.state));
  }
  // [end:on_connection]

  // [start:on_quoting]
  void on_quoting(auto& ctx, bool enabled) noexcept {
    if (!enabled) return;  // paused: the engine has pulled the quotes
    for (const Instrument& inst : ctx.instruments()) on_book(ctx, inst.id, ctx.book(inst.id));
  }
  // [end:on_quoting]

  // [start:on_timer]
  void on_timer(auto& ctx, TimerId id, std::uint64_t /*tag*/) noexcept {
    if (id != report_timer_) return;
    FASTMM_LOG_INFO("first_mm: fills={} late_fills={} disconnects={} net_pnl={}",
                    fills_,
                    late_fills_,
                    disconnects_,
                    ctx.portfolio().net);
  }
  // [end:on_timer]

  [[nodiscard]] std::uint64_t fills() const noexcept { return fills_; }
  [[nodiscard]] std::uint64_t disconnects() const noexcept { return disconnects_; }

 private:
  TimerId report_timer_{};
  std::uint64_t fills_ = 0;
  std::uint64_t late_fills_ = 0;
  std::uint64_t disconnects_ = 0;
};

// [start:verify]
static_assert(verify_strategy<FirstMM>());  // a hook with a wrong signature fails here
// [end:verify]

}  // namespace tutorial
