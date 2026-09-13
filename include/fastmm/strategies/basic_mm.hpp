#pragma once
// BasicMM (8.6): symmetric quotes around mid, half_spread_bps wide, skewed by inventory,
// with a per-side inventory cap. Integer math throughout: bps are carried as centi-bps so
// fractional config values keep two decimals.
#include "fastmm/core/book/book_view.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/strategies/strategy.hpp"

#include <cmath>
#include <cstdint>
#include <string_view>

namespace fastmm {

struct BasicMMParams {
  FASTMM_PARAMS(BasicMMParams)
  FASTMM_PARAM(double, half_spread_bps, 5.0, 0.0, 10000.0, "half spread around mid, basis points")
  FASTMM_PARAM(double,
               skew_bps_per_unit,
               1.0,
               0.0,
               10000.0,
               "shift both quotes by this many bps per quote_qty of inventory")
  FASTMM_PARAM(double, quote_qty, 0.01, 0.0, 1e9, "quantity per level (base units)")
  FASTMM_PARAM(double,
               max_inventory,
               0.1,
               0.0,
               1e9,
               "stop quoting the side that would grow |position| past this")
  FASTMM_PARAM(
      int, requote_threshold_ticks, 1, 0, 1000000, "ignore mid moves smaller than this many ticks")
  FASTMM_PARAM(int,
               pull_on_stale_ms,
               2000,
               0,
               3600000,
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
    // Config doubles -> fixed point exactly once, here.
    half_spread_cbps_ = static_cast<std::int64_t>(std::llround(params_.half_spread_bps * 100.0));
    skew_cbps_ = static_cast<std::int64_t>(std::llround(params_.skew_bps_per_unit * 100.0));
    quote_qty_ = Qty::from_double(params_.quote_qty);
    max_inventory_ = Qty::from_double(params_.max_inventory);
    for (auto& m : last_mid_) m = Price{};
    if (params_.pull_on_stale_ms > 0)
      stale_timer_ = ctx.add_timer(milliseconds(100), true, kStaleTimer);
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
    if (last.is_positive() &&
        (mid - last).abs().raw < params_.requote_threshold_ticks * inst.tick.raw)
      return;
    requote(ctx, id, book, inst);
  }

  template <class Ctx>
  void on_fill(Ctx& ctx, const OmsUpdate& u, const OrderFillMsg&) noexcept {
    // Inventory changed: re-skew immediately (bypasses the mid-move threshold).
    const InstrumentId id = u.known ? u.order.instrument : InstrumentId{};
    if (!id.valid() || !ctx.instruments().contains(id)) return;
    const auto& book = ctx.book(id);
    if (book.is_valid()) requote(ctx, id, book, ctx.instrument(id));
  }

  template <class Ctx>
  void on_timer(Ctx& ctx, TimerId, std::uint64_t user_data) noexcept {
    if (user_data != kStaleTimer) return;
    const Timestamp now = ctx.now();
    for (const Instrument& inst : ctx.instruments()) {
      const auto& book = ctx.book(inst.id);
      if (book.last_update().valid() &&
          (now - book.last_update()).millis() > params_.pull_on_stale_ms) {
        ctx.pull_quotes(inst.id);
        last_mid_[inst.id.value] = Price{};
      }
    }
  }

  // Post-only quotes must not cross the market. Inventory skew can push the unwinding side
  // through the touch (a skewed bid above the best ask); the venue would reject it and that side
  // would stop quoting. Shift the whole ladder back so level 0 sits one tick inside the touch,
  // keeping the spacing between levels. An empty opposite side (price 0) imposes no limit.
  static void clamp_to_touch(DesiredQuotes& q,
                             Price best_bid,
                             Price best_ask,
                             Price tick) noexcept {
    if (!q.bids.empty() && best_ask.is_positive()) {
      const Price limit = best_ask - tick;
      if (q.bids[0].price > limit) {
        const Price shift = q.bids[0].price - limit;
        for (auto& l : q.bids) l.price = l.price - shift;
        while (!q.bids.empty() && !q.bids[q.bids.size() - 1].price.is_positive()) q.bids.pop_back();
      }
    }
    if (!q.asks.empty() && best_bid.is_positive()) {
      const Price limit = best_bid + tick;
      if (q.asks[0].price < limit) {
        const Price shift = limit - q.asks[0].price;
        for (auto& l : q.asks) l.price = l.price + shift;
      }
    }
  }

  // Pure quoting function; exposed for deterministic tests.
  [[nodiscard]] DesiredQuotes compute_quotes(Price mid,
                                             Qty position,
                                             const Instrument& inst) const noexcept {
    DesiredQuotes q;
    if (quote_qty_.is_zero()) return q;
    const std::int64_t inv_units = position.raw / quote_qty_.raw;  // signed, truncating
    // offsets in price units: mid * cbps / 1e6
    const auto half = Price::from_raw(
        static_cast<std::int64_t>(static_cast<Int128>(mid.raw) * half_spread_cbps_ / 1'000'000));
    const auto skew = Price::from_raw(static_cast<std::int64_t>(
        static_cast<Int128>(mid.raw) * skew_cbps_ * -inv_units / 1'000'000));
    const Price centre = mid + skew;
    const Price step = Price::from_raw(inst.tick.raw * params_.level_step_ticks);
    const bool can_buy = max_inventory_.is_zero() || position + quote_qty_ <= max_inventory_;
    const bool can_sell = max_inventory_.is_zero() || position - quote_qty_ >= -max_inventory_;
    const Qty qty = inst.round_qty(quote_qty_);
    for (int l = 0; l < params_.levels; ++l) {
      const Price off = Price::from_raw(step.raw * l);
      if (can_buy) {
        Price bid = inst.round_price(centre - half - off, Side::Buy);
        if (bid.is_positive()) static_cast<void>(q.bids.push_back(Level{bid, qty}));
      }
      if (can_sell) {
        Price ask = inst.round_price(centre + half + off, Side::Sell);
        static_cast<void>(q.asks.push_back(Level{ask, qty}));
      }
    }
    // never cross ourselves at level 0
    if (!q.bids.empty() && !q.asks.empty() && q.bids[0].price >= q.asks[0].price) {
      q.asks[0].price = q.bids[0].price + inst.tick;
    }
    return q;
  }

 private:
  template <class Ctx, class Book>
  void requote(Ctx& ctx, InstrumentId id, const Book& book, const Instrument& inst) noexcept {
    const Price mid = book.mid();
    DesiredQuotes q = compute_quotes(mid, ctx.position(id).qty, inst);
    clamp_to_touch(q, book.best_bid().price, book.best_ask().price, inst.tick);
    ctx.set_quotes(id, q);
    last_mid_[id.value] = mid;
  }

  std::int64_t half_spread_cbps_ = 0;
  std::int64_t skew_cbps_ = 0;
  Qty quote_qty_{};
  Qty max_inventory_{};
  TimerId stale_timer_{};
  Price last_mid_[kMaxInstruments] = {};
};

static_assert(StrategyLike<BasicMM>);

}  // namespace fastmm
