#pragma once
// Quoting helpers for strategies (ADR-0012): integer-only, noexcept, no allocation.
//
//   mid(bid, ask)                          (bid + ask) / 2, truncated like the books' mid()
//   microprice(bid_level, ask_level)       size-weighted mid of the touch
//   spread_ratio(bid, ask)                 (ask - bid) / mid as a Ratio
//   away_from(ref, side, dist)             a bid dist below ref, an ask dist above
//   inventory_allows(side, pos, qty, lim)  may this side add qty without passing the limit
//   keep_passive(q, best_bid, best_ask, tick)
//                                          shift a ladder so level 0 is one tick inside the touch
//   fit_side(levels, room, inst)           cut a side to a total quantity
//   fit_to_balance(ctx, id, inst, q)       cut both sides to what the account's balance covers
//
// Together with Instrument::ticks(n), DesiredQuotes::bid/ask/uncross and the Ratio operators and
// literals in core/fixed_point.hpp.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/quote_manager.hpp"

#include <cstdint>

namespace fastmm {

[[nodiscard]] constexpr Price mid(Price bid, Price ask) noexcept {
  return Price::from_raw((bid.raw + ask.raw) / 2);
}

// Size-weighted microprice of the touch, truncated toward zero. With no size on either side it is
// the plain mid.
[[nodiscard]] constexpr Price microprice(Level bid, Level ask) noexcept {
  const Int128 w = static_cast<Int128>(bid.qty.raw) + ask.qty.raw;
  if (w <= 0) return mid(bid.price, ask.price);
  const Int128 num = static_cast<Int128>(bid.price.raw) * ask.qty.raw +
                     static_cast<Int128>(ask.price.raw) * bid.qty.raw;
  return Price::from_raw(static_cast<std::int64_t>(num / w));
}

// (ask - bid) / mid; zero when the mid is not positive. 1 bp == Ratio raw 10'000.
[[nodiscard]] constexpr Ratio spread_ratio(Price bid, Price ask) noexcept {
  const Price m = mid(bid, ask);
  if (!m.is_positive()) return Ratio{};
  return ratio(ask - bid, m);
}

// A price `dist` away from `ref` on the passive side: Buy -> ref - dist, Sell -> ref + dist.
[[nodiscard]] constexpr Price away_from(Price ref, Side side, Price dist) noexcept {
  return side == Side::Buy ? ref - dist : ref + dist;
}

// Whether a fill of `qty` on `side` keeps the position within +-limit: Buy needs
// position + qty <= limit, Sell needs position - qty >= -limit. A zero limit means no cap. A side
// that reduces the position is allowed as long as the result stays within the limit.
[[nodiscard]] constexpr bool inventory_allows(Side side,
                                              Qty position,
                                              Qty qty,
                                              Qty limit) noexcept {
  if (limit.is_zero()) return true;
  return side == Side::Buy ? position + qty <= limit : position - qty >= -limit;
}

// Post-only quotes must not cross the market. Inventory skew can push the unwinding side through
// the touch (a skewed bid above the best ask); the venue would reject it and that side would stop
// quoting. Shift the whole ladder back so level 0 sits one tick inside the touch, keeping the
// spacing between levels; bid levels pushed to a non-positive price are removed. An empty opposite
// side (price 0) imposes no limit.
inline void keep_passive(DesiredQuotes& q, Price best_bid, Price best_ask, Price tick) noexcept {
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

// Cuts one side of a ladder to `room` in total, keeping the levels closest to the mid: each level
// is rounded down to the lot, and one left under the lot, the instrument's min_qty or its
// min_notional ends the side (the venue would refuse it).
inline void fit_side(StaticVector<Level, kMaxQuoteLevels>& levels,
                     Qty room,
                     const Instrument& inst) noexcept {
  Qty left = room;
  std::size_t keep = 0;
  for (; keep < levels.size(); ++keep) {
    Level& l = levels[keep];
    if (l.qty > left) l.qty = inst.round_qty(left);
    if (!l.qty.is_positive() || l.qty < inst.min_qty) break;
    if (inst.min_notional.is_positive() && inst.notional(l.price, l.qty) < inst.min_notional) break;
    left = left - l.qty;
  }
  while (levels.size() > keep) levels.pop_back();
}

// Cuts both sides of a ladder to what the account's balance on the instrument's venue covers
// (StrategyContext::balance_room, priced at the side's dearest level). What the side's resting
// orders hold counts as room: the ladder replaces them. A side whose balance the venue has not
// reported is left as it is. Before any venue reports balances it is one test.
template <class Ctx>
FASTMM_NOINLINE void fit_to_reported(Ctx& ctx,
                                     InstrumentId id,
                                     const Instrument& inst,
                                     DesiredQuotes& q) noexcept {
  for (const Side side : {Side::Buy, Side::Sell}) {
    auto& levels = side == Side::Buy ? q.bids : q.asks;
    if (levels.empty()) continue;
    const Price dear = side == Side::Buy ? levels[0].price : levels[levels.size() - 1].price;
    const Qty room = ctx.balance_room(id, side, dear);
    if (room == Qty::max()) continue;
    fit_side(levels, room + ctx.open_qty(id, side), inst);
  }
}
template <class Ctx>
FASTMM_FORCE_INLINE void fit_to_balance(Ctx& ctx,
                                        InstrumentId id,
                                        const Instrument& inst,
                                        DesiredQuotes& q) noexcept {
  if (FASTMM_UNLIKELY(ctx.balances_live())) fit_to_reported(ctx, id, inst, q);
}

}  // namespace fastmm
