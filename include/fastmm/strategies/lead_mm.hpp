#pragma once
// LeadMM: passive quotes on a thin pair priced off a liquid leader pair.
//
//   fair      leader_mid / fx_mid       (fx_invert: leader_mid * fx_mid; fx = -1: leader_mid)
//   edge      bid: (fair - bid) / bid   ask: (ask - fair) / ask
//   quote     a side at the target's touch (improve: one tick inside) when its edge is at least
//             edge_min_bps + skew_bps_per_unit * q (bid) or edge_min_bps - skew_bps_per_unit * q
//             (ask), with q = position / quote_qty, fractional and signed.
//
// Roles are instrument indices in [[instruments]] (the InstrumentId; parameters have no string
// type): `target` is quoted, `leader` and `fx` are only read and are normally listed with
// enabled = false. The indices are checked against the table before the session starts and read
// once in on_start; updating them on a running engine has no effect.
//
// Top of book per role: the depth book or the latest BookTicker (@bookTicker / @bestBidAsk are
// real time, depth is throttled to 100 ms), whichever is newer. A ticker that is crossed or has a
// zero size is ignored. Leader and target tickers requote at once; fx tickers are only stored.
//
// The fair value is valid only while the leader's top is valid and at most max_leader_age_ms old,
// and the fx top at most fx_max_age_ms old; otherwise every target quote is pulled. The target is
// not age-checked: a thin pair can be quiet for minutes, and the edge test is against the leader.
//
// Imbalance (imb_bps, 0 = off): the fair value used for the target is
//   fair * (1 + imb_bps / 1e4 * imb),  imb = (bid - ask) / (bid + ask)
// over the target's top imb_levels levels per side, from the top picked above (the ticker's sizes
// for level 1 when it is the newer, deeper levels from the depth book). Our own quantity in the
// feed (ctx.own_qty at the ticker's or the book's venue time; zero in the simulator) is taken out
// of each level first; a side left empty makes imb +-1 toward the other side, both empty 0.
//
// Hysteresis: a resting quote whose price is still the one the rule picks keeps its side while its
// edge is at least the threshold minus hysteresis_bps. The strategy never improves on its own
// resting order (live books include it), and never quotes a bid at or above the best ask or an ask
// at or below the best bid. The QuoteManager's min_requote_ticks / min_requote_interval_ms still
// apply to every change.
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
#include <optional>
#include <string>
#include <string_view>

namespace fastmm {

struct LeadMMParams {
  FASTMM_PARAMS(LeadMMParams)
  FASTMM_PARAM(int, target, 0, 0, 255, "quoted instrument: its index in [[instruments]]")
  FASTMM_PARAM(int, leader, 1, 0, 255, "instrument whose mid is the fair value (index)")
  FASTMM_PARAM(int, fx, 2, -1, 255, "instrument converting the leader's price (index, -1 = none)")
  FASTMM_PARAM(bool, fx_invert, false, false, true, "multiply by the fx mid instead of dividing")
  FASTMM_PARAM_BPS(edge_min_bps, 0.3_bps, 0_bps, 10000_bps, "minimum edge to fair, bps of price")
  FASTMM_PARAM_BPS(hysteresis_bps,
                   0.1_bps,
                   0_bps,
                   10000_bps,
                   "a resting quote stays until its edge drops this far below the minimum")
  FASTMM_PARAM_BPS(skew_bps_per_unit,
                   0.2_bps,
                   0_bps,
                   10000_bps,
                   "added to the buy threshold, taken off the sell one, per quote_qty held")
  FASTMM_PARAM(Qty, quote_qty, 0.01_qty, 0_qty, 1000000000_qty, "quantity per side (base units)")
  FASTMM_PARAM(Qty,
               max_inventory,
               0.1_qty,
               0_qty,
               1000000000_qty,
               "stop quoting the side that would grow |position| past this (0 = no cap)")
  FASTMM_PARAM_MS(max_leader_age_ms,
                  milliseconds(500),
                  milliseconds(1),
                  milliseconds(3600000),
                  "pull quotes when the leader's top of book is older than this")
  FASTMM_PARAM_MS(fx_max_age_ms,
                  milliseconds(60000),
                  milliseconds(1),
                  milliseconds(3600000),
                  "pull quotes when the fx top of book is older than this")
  FASTMM_PARAM(bool,
               bbo_by_update_id,
               true,
               false,
               true,
               "ticker vs depth: newer by venue update id (Binance) instead of by timestamp")
  FASTMM_PARAM(
      bool, improve, false, false, true, "quote one tick inside a spread wider than 1 tick")
  FASTMM_PARAM_BPS(imb_bps,
                   0_bps,
                   -100_bps,
                   100_bps,
                   "fair shift per unit of target book imbalance, bps (0 = off)")
  FASTMM_PARAM(int, imb_levels, 1, 1, 8, "target levels per side in the imbalance")

  [[nodiscard]] std::optional<std::string> validate() const {
    if (leader == target || fx == target || fx == leader)
      return "target, leader and fx must be different instruments";
    return std::nullopt;
  }
};

class LeadMM : public StrategyBase<LeadMMParams> {
 public:
  static constexpr std::string_view name() noexcept { return "lead_mm"; }
  static constexpr std::uint64_t kStaleTimer = 0x5741'4c45;  // "STALE"

  // Before the engine is built: every role must name an instrument, and the target must be
  // tradable. Startup only.
  [[nodiscard]] std::optional<std::string> check_instruments(const InstrumentTable& t) const {
    const LeadMMParams& p = params();
    const auto missing = [&](int i) { return i >= 0 && static_cast<std::size_t>(i) >= t.size(); };
    if (missing(p.target) || missing(p.leader) || missing(p.fx)) {
      return "target, leader and fx must be indices into the " + std::to_string(t.size()) +
             " configured instruments";
    }
    if (!t.get(InstrumentId{static_cast<std::uint32_t>(p.target)}).enabled())
      return "the target instrument is disabled";
    return std::nullopt;
  }

  template <class Ctx>
  void on_start(Ctx& ctx) noexcept {
    const LeadMMParams& p = params();
    target_ = InstrumentId{static_cast<std::uint32_t>(p.target)};
    leader_ = InstrumentId{static_cast<std::uint32_t>(p.leader)};
    has_fx_ = p.fx >= 0;
    fx_ = InstrumentId{static_cast<std::uint32_t>(has_fx_ ? p.fx : 0)};
    pulled_ = false;
    for (Top& t : ticker_) t = Top{};
    stale_timer_ = ctx.every(milliseconds(100), kStaleTimer);
  }

  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book&) noexcept {
    if (id == target_ || id == leader_ || (has_fx_ && id == fx_)) requote(ctx);
  }

  template <class Ctx>
  void on_book_ticker(Ctx& ctx, InstrumentId id, const BookTickerMsg& m) noexcept {
    const int r = role(id);
    if (r < 0 || !m.bid_qty.is_positive() || !m.ask_qty.is_positive() || !m.bid_px.is_positive() ||
        m.ask_px <= m.bid_px)
      return;
    ticker_[r] = Top{m.bid_px,
                     m.ask_px,
                     m.hdr.venue_seq,
                     m.hdr.exch_ts.valid() ? m.hdr.exch_ts : m.hdr.recv_ts,
                     true,
                     m.bid_qty,
                     m.ask_qty,
                     true};
    if (r != kFx) requote(ctx);
  }

  template <class Ctx>
  void on_fill(Ctx& ctx, const Fill& fill) noexcept {
    if (fill.instrument == target_) requote(ctx);  // inventory changed: re-gate and re-skew
  }

  // A leader that stops updating sends no book events: the timer is what pulls the quotes then.
  template <class Ctx>
  void on_timer(Ctx& ctx, TimerId, std::uint64_t user_data) noexcept {
    if (user_data == kStaleTimer && !pulled_ && !fair(ctx)) pull(ctx);
  }

  // The engine pulls a venue's quotes when a connection drops; requote at once when it is Live
  // again (a quiet target book could otherwise leave the strategy unquoted).
  // Any change forgets that venue's tickers: after a gap the depth book (resynced) is the truth.
  template <class Ctx>
  void on_connection(Ctx& ctx, const ConnectionStateMsg& m) noexcept {
    const InstrumentId ids[kRoles] = {target_, leader_, fx_};
    for (int r = 0; r < kRoles; ++r) {
      if (ctx.instrument(ids[r]).venue == m.hdr.venue) ticker_[r] = Top{};
    }
    if (m.state == ConnState::Live && ctx.instrument(target_).venue == m.hdr.venue) requote(ctx);
  }

  template <class Ctx>
  void on_quoting(Ctx& ctx, bool enabled) noexcept {
    if (enabled) requote(ctx);
  }

  // The leader mid in the target's price unit. fx_mid is ignored without an fx instrument.
  [[nodiscard]] Price fair_value(Price leader_mid, Price fx_mid) const noexcept {
    if (!has_fx_) return leader_mid;
    if (params().fx_invert)
      return Price::from_raw(detail::mul_div<kFixedScale>(leader_mid.raw, fx_mid.raw));
    if (!fx_mid.is_positive()) return Price{};
    return Price::from_raw(detail::mul_div(leader_mid.raw, kFixedScale, fx_mid.raw));
  }

  // Pure quoting function; exposed for deterministic tests. own_bid / own_ask: the price of the
  // resting quote on that side, zero when there is none.
  //
  // Edges are Ratios (1e-8, i.e. 1e-4 bp), computed in fixed point: exact enough for a threshold
  // in tenths of a bp, and the gating then does not depend on floating-point evaluation order.
  [[nodiscard]] DesiredQuotes compute_quotes(Price fair,
                                             Price best_bid,
                                             Price best_ask,
                                             Price own_bid,
                                             Price own_ask,
                                             Qty position,
                                             const Instrument& inst) const noexcept {
    const LeadMMParams& p = params();
    DesiredQuotes q;
    const Qty qty = inst.round_qty(p.quote_qty);
    if (!qty.is_positive() || !fair.is_positive() || !best_bid.is_positive() ||
        best_ask <= best_bid)
      return q;
    const Ratio skew = inventory_skew(position);
    const Ratio buy_min = p.edge_min_bps + skew;
    const Ratio sell_min = p.edge_min_bps - skew;
    const bool can_buy = inventory_allows(Side::Buy, position, p.quote_qty, p.max_inventory);
    const bool can_sell = inventory_allows(Side::Sell, position, p.quote_qty, p.max_inventory);
    Price bid =
        can_buy ? pick(Side::Buy, fair, best_bid, best_ask, own_bid, buy_min, p.improve, inst.tick)
                : Price{};
    Price ask =
        can_sell
            ? pick(Side::Sell, fair, best_bid, best_ask, own_ask, sell_min, p.improve, inst.tick)
            : Price{};
    // Both sides improved into the same tick of a 2-tick spread: join on both instead.
    if (bid.is_positive() && ask.is_positive() && bid >= ask) {
      bid = pick(Side::Buy, fair, best_bid, best_ask, own_bid, buy_min, false, inst.tick);
      ask = pick(Side::Sell, fair, best_bid, best_ask, own_ask, sell_min, false, inst.tick);
    }
    q.bid(bid, qty);
    q.ask(ask, qty);
    return q;
  }

  // skew_bps_per_unit * position / quote_qty, truncated toward zero.
  [[nodiscard]] Ratio inventory_skew(Qty position) const noexcept {
    const LeadMMParams& p = params();
    if (!p.quote_qty.is_positive()) return Ratio{};
    const Int128 r = static_cast<Int128>(p.skew_bps_per_unit.raw) * position.raw / p.quote_qty.raw;
    constexpr Int128 kLimit = static_cast<Int128>(kRatioPerBp) * 1'000'000;  // +-1e6 bps
    return Ratio::from_raw(static_cast<std::int64_t>(r > kLimit    ? kLimit
                                                     : r < -kLimit ? -kLimit
                                                                   : r));
  }

  // (bid - ask) / (bid + ask); a side with nothing is -1 or +1 toward the other, both empty 0.
  [[nodiscard]] static Ratio imbalance(Qty bid, Qty ask) noexcept {
    if (!bid.is_positive() && !ask.is_positive()) return Ratio{};
    if (!bid.is_positive()) return Ratio::from_raw(-kFixedScale);
    if (!ask.is_positive()) return Ratio::from_raw(kFixedScale);
    return ratio(bid - ask, bid + ask);
  }
  // fair * (1 + imb_bps / 1e4 * imb), truncated toward zero.
  [[nodiscard]] Price shift_fair(Price fair, Ratio imb) const noexcept {
    return fair + fair * (params().imb_bps * imb);
  }

  // The edge of a quote at `px` on `side`: (fair - px) / px for a bid, (px - fair) / px for an ask.
  [[nodiscard]] static Ratio edge(Side side, Price fair, Price px) noexcept {
    return side == Side::Buy ? ratio(fair - px, px) : ratio(px - fair, px);
  }

 private:
  enum : int { kTarget = 0, kLeader = 1, kFx = 2, kRoles = 3 };
  struct Top {
    Price bid{};
    Price ask{};
    std::uint64_t id = 0;  // venue update id, 0 = none
    Timestamp ts{};
    bool valid = false;
    Qty bid_qty{};
    Qty ask_qty{};
    bool ticker = false;  // from the BookTicker, not the depth book
    [[nodiscard]] Price mid() const noexcept { return fastmm::mid(bid, ask); }
  };

  [[nodiscard]] int role(InstrumentId id) const noexcept {
    if (id == target_) return kTarget;
    if (id == leader_) return kLeader;
    if (has_fx_ && id == fx_) return kFx;
    return -1;
  }

  // The newer of the depth book's top and the role's ticker. Update ids when both have one and
  // bbo_by_update_id (Binance puts the same order book updateId on both), else timestamps.
  template <class Ctx>
  [[nodiscard]] Top top(Ctx& ctx, InstrumentId id, int r) const noexcept {
    const auto& b = ctx.book(id);
    const Level bb = b.best_bid();
    const Level ba = b.best_ask();
    const Top book{bb.price, ba.price, b.seq(), b.last_update(), b.is_valid(), bb.qty, ba.qty};
    const Top& t = ticker_[r];
    if (!t.valid) return book;
    if (!book.valid) return t;
    const bool by_id = params().bbo_by_update_id && t.id != 0 && book.id != 0;
    const bool ticker_newer = by_id ? t.id > book.id : t.ts > book.ts;
    return ticker_newer ? t : book;
  }

  // The price for one side, or zero. Candidates in order: one tick inside the touch (improve, a
  // spread wider than a tick, and the touch is not our own order), then the touch. A candidate
  // equal to the resting quote only needs min - hysteresis.
  [[nodiscard]] Price pick(Side side,
                           Price fair,
                           Price best_bid,
                           Price best_ask,
                           Price own,
                           Ratio min_edge,
                           bool improve,
                           Price tick) const noexcept {
    const bool buy = side == Side::Buy;
    const Price touch = buy ? best_bid : best_ask;
    const auto passes = [&](Price px) {
      if (!px.is_positive() || (buy ? px >= best_ask : px <= best_bid)) return false;
      const Ratio need = px == own ? min_edge - params().hysteresis_bps : min_edge;
      return edge(side, fair, px) >= need;
    };
    if (improve && touch != own && best_ask - best_bid > tick) {
      const Price inside = buy ? touch + tick : touch - tick;
      if (passes(inside)) return inside;
    }
    return passes(touch) ? touch : Price{};
  }

  // The fair value, or nothing while the leader's or fx's top is invalid or too old.
  template <class Ctx>
  [[nodiscard]] std::optional<Price> fair(Ctx& ctx) const noexcept {
    const Timestamp now = ctx.now();
    const auto fresh = [&](const Top& t, Duration max_age) {
      return t.valid && t.ts.valid() && now - t.ts <= max_age;
    };
    const Top leader = top(ctx, leader_, kLeader);
    if (!fresh(leader, params().max_leader_age_ms)) return std::nullopt;
    Price fx_mid{};
    if (has_fx_) {
      const Top fx = top(ctx, fx_, kFx);
      if (!fresh(fx, params().fx_max_age_ms)) return std::nullopt;
      fx_mid = fx.mid();
    }
    const Price f = fair_value(leader.mid(), fx_mid);
    if (!f.is_positive()) return std::nullopt;
    return f;
  }

  // The target's imbalance over imb_levels levels: level 1 from `top` (ticker or depth book),
  // further levels from the depth book beyond it; our own quantity in the feed taken out of each.
  template <class Ctx>
  [[nodiscard]] Ratio target_imbalance(Ctx& ctx, const Top& top) const noexcept {
    const LeadMMParams& p = params();
    const auto& b = ctx.book(target_);
    Qty sums[2];
    for (const Side side : {Side::Buy, Side::Sell}) {
      const bool buy = side == Side::Buy;
      Qty& sum = sums[buy ? 0 : 1];
      const auto add = [&](Price px, Qty q, Timestamp at) {
        const Qty own = ctx.own_qty(target_, side, px, at);
        sum += own >= q ? Qty{} : q - own;
      };
      const Price first = buy ? top.bid : top.ask;
      add(first, buy ? top.bid_qty : top.ask_qty, top.ts);
      int n = 1;
      const std::size_t depth = b.depth(side);
      for (std::size_t i = 0; i < depth && n < p.imb_levels; ++i) {
        const Level l = b.level(side, i);
        if (buy ? l.price >= first : l.price <= first) continue;  // at or better than level 1
        add(l.price, l.qty, b.last_update());
        ++n;
      }
    }
    return imbalance(sums[0], sums[1]);
  }

  template <class Ctx>
  void pull(Ctx& ctx) noexcept {
    ctx.pull_quotes(target_);
    pulled_ = true;
  }

  template <class Ctx>
  void requote(Ctx& ctx) noexcept {
    const Top book = top(ctx, target_, kTarget);
    const std::optional<Price> f = fair(ctx);
    if (!book.valid || !f) {
      if (!pulled_) pull(ctx);
      return;
    }
    const auto own = [&](Side s) {
      const Order* o = ctx.working_quote(target_, s);
      return o != nullptr ? o->price : Price{};
    };
    Price used = *f;
    if (params().imb_bps.raw != 0) used = shift_fair(used, target_imbalance(ctx, book));
    const DesiredQuotes q = compute_quotes(used,
                                           book.bid,
                                           book.ask,
                                           own(Side::Buy),
                                           own(Side::Sell),
                                           ctx.position(target_).qty,
                                           ctx.instrument(target_));
    // Remember a pull only while the quotes are ignored (quoting disabled): the engine has pulled.
    pulled_ = !ctx.set_quotes(target_, q);
  }

  InstrumentId target_{};
  InstrumentId leader_{};
  InstrumentId fx_{};
  bool has_fx_ = false;
  bool pulled_ = false;
  Top ticker_[kRoles] = {};
  TimerId stale_timer_{};
};

static_assert(StrategyLike<LeadMM>);
static_assert(verify_strategy<LeadMM>());

}  // namespace fastmm
