#pragma once
// BasicMM (8.6): symmetric quotes around mid, half_spread_bps wide, skewed by inventory,
// with a per-side inventory cap. Integer math throughout: bps parameters are Ratios (0.0001 bp
// resolution) and quantities are parsed exactly.
#include "fastmm/core/book/book_view.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/strategies/quoting.hpp"
#include "fastmm/strategies/strategy.hpp"

#include <cstdint>
#include <string_view>

namespace fastmm {

struct BasicMMParams {
  FASTMM_PARAMS(BasicMMParams)
  FASTMM_PARAM_BPS(half_spread_bps, 5_bps, 0_bps, 10000_bps, "half spread around mid, basis points")
  FASTMM_PARAM_BPS(skew_bps_per_unit,
                   1_bps,
                   0_bps,
                   10000_bps,
                   "shift both quotes by this many bps per quote_qty of inventory")
  FASTMM_PARAM(Qty, quote_qty, 0.01_qty, 0_qty, 1000000000_qty, "quantity per level (base units)")
  FASTMM_PARAM(Qty,
               max_inventory,
               0.1_qty,
               0_qty,
               1000000000_qty,
               "stop quoting the side that would grow |position| past this (0 = no cap)")
  FASTMM_PARAM(
      int, requote_threshold_ticks, 1, 0, 1000000, "ignore mid moves smaller than this many ticks")
  FASTMM_PARAM_MS(pull_on_stale_ms,
                  milliseconds(2000),
                  milliseconds(0),
                  milliseconds(3600000),
                  "pull quotes when the book is older than this (0 = never)")
  FASTMM_PARAM(int, levels, 1, 1, 8, "quote levels per side")
  FASTMM_PARAM(int, level_step_ticks, 1, 1, 100000, "tick distance between successive levels")
};

class BasicMM : public StrategyBase<BasicMMParams> {
 public:
  static constexpr std::string_view name() noexcept { return "basic_mm"; }
  static constexpr std::uint64_t kStaleTimer = 0x5741'4c45;  // "STALE"

  template <class Ctx>
  void on_start(Ctx& ctx) noexcept {
    for (auto& m : last_mid_) m = Price{};
    if (params().pull_on_stale_ms > Duration{})
      stale_timer_ = ctx.every(milliseconds(100), kStaleTimer);
  }

  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book& book) noexcept {
    if (!book.is_valid()) {
      ctx.pull_quotes(id);
      last_mid_[id.value] = Price{};
      return;
    }
    const Price mid = book.mid();
    const Instrument& inst = ctx.instrument(id);
    const Price& last = last_mid_[id.value];
    if (last.is_positive() && (mid - last).abs() < inst.ticks(params().requote_threshold_ticks))
      return;
    requote(ctx, id, book, inst);
  }

  template <class Ctx>
  void on_fill(Ctx& ctx, const Fill& fill) noexcept {
    // Inventory changed: re-skew immediately (bypasses the mid-move threshold).
    const auto& book = ctx.book(fill.instrument);
    if (book.is_valid()) requote(ctx, fill.instrument, book, ctx.instrument(fill.instrument));
  }

  template <class Ctx>
  void on_timer(Ctx& ctx, TimerId, std::uint64_t user_data) noexcept {
    if (user_data != kStaleTimer) return;
    const Timestamp now = ctx.now();
    for (const Instrument& inst : ctx.instruments()) {
      const auto& book = ctx.book(inst.id);
      if (book.last_update().valid() &&
          (now - book.last_update()).millis() > params().pull_on_stale_ms.millis()) {
        ctx.pull_quotes(inst.id);
        last_mid_[inst.id.value] = Price{};
      }
    }
  }

  // The engine pulls a venue's quotes when a connection drops. Forget the last quoted mid so the
  // next book update requotes even if the mid has not moved, and requote at once when the venue
  // is Live again (otherwise a quiet book could leave the strategy unquoted indefinitely).
  template <class Ctx>
  void on_connection(Ctx& ctx, const ConnectionStateMsg& m) noexcept {
    for (const Instrument& inst : ctx.instruments()) {
      if (inst.venue != m.hdr.venue) continue;
      last_mid_[inst.id.value] = Price{};
      if (m.state != ConnState::Live) continue;
      const auto& book = ctx.book(inst.id);
      if (book.is_valid()) requote(ctx, inst.id, book, inst);
    }
  }

  // Quoting was paused or resumed (control pull, kill switch, reconciliation). Forget the quoted
  // mids, and when quoting is back requote at once, even if the mid has not moved.
  template <class Ctx>
  void on_quoting(Ctx& ctx, bool enabled) noexcept {
    for (const Instrument& inst : ctx.instruments()) {
      last_mid_[inst.id.value] = Price{};
      if (!enabled) continue;
      const auto& book = ctx.book(inst.id);
      if (book.is_valid()) requote(ctx, inst.id, book, inst);
    }
  }

  // Pure quoting function; exposed for deterministic tests. Truncation matches the centi-bps
  // formula it replaced (mid.raw * cbps / 1e6 == mid.raw * (cbps * 100) / 1e8).
  [[nodiscard]] DesiredQuotes compute_quotes(Price mid,
                                             Qty position,
                                             const Instrument& inst) const noexcept {
    const BasicMMParams& p = params();
    DesiredQuotes q;
    if (p.quote_qty.is_zero()) return q;
    const std::int64_t inventory_units = position / p.quote_qty;  // signed, truncating
    const Price half = mid * p.half_spread_bps;
    const Price centre = mid - mid * (p.skew_bps_per_unit * inventory_units);
    const Price step = inst.ticks(p.level_step_ticks);
    const Qty qty = inst.round_qty(p.quote_qty);
    const bool can_buy = inventory_allows(Side::Buy, position, p.quote_qty, p.max_inventory);
    const bool can_sell = inventory_allows(Side::Sell, position, p.quote_qty, p.max_inventory);
    for (int l = 0; l < p.levels; ++l) {
      if (can_buy) q.bid(inst.round_price(centre - half - step * l, Side::Buy), qty);
      if (can_sell) q.ask(inst.round_price(centre + half + step * l, Side::Sell), qty);
    }
    q.uncross(inst.tick);
    return q;
  }

 private:
  template <class Ctx, class Book>
  void requote(Ctx& ctx, InstrumentId id, const Book& book, const Instrument& inst) noexcept {
    const Price mid = book.mid();
    DesiredQuotes q = compute_quotes(mid, ctx.position(id).qty, inst);
    keep_passive(q, book.best_bid().price, book.best_ask().price, inst.tick);
    // Remember the mid only if the quotes were taken; while quoting is disabled they are ignored.
    last_mid_[id.value] = ctx.set_quotes(id, q) ? mid : Price{};
  }

  TimerId stale_timer_{};
  Price last_mid_[kMaxInstruments] = {};
};

static_assert(StrategyLike<BasicMM>);
static_assert(verify_strategy<BasicMM>());

}  // namespace fastmm
