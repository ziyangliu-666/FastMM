#pragma once
// Pre-trade risk (5.7). check_new() evaluates a fixed, documented sequence of integer
// comparisons and returns the first RejectReason (None passes). Collar and fat-finger
// bounds are precomputed on every book/trade update so the check itself is branch + compare.
//
// Kill switch: bit 0 = global, bit (1 + venue) = per venue; settable from any thread.
// Cancels are always allowed, even when killed.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/fx.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/order.hpp"
#include "fastmm/core/position.hpp"
#include "fastmm/core/risk_limits.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/underlying.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace fastmm {

// Integer token bucket: tokens in millionths, refilled from elapsed ns.
class TokenBucket {
 public:
  static constexpr std::int64_t kScale = 1'000'000;
  TokenBucket() noexcept = default;
  TokenBucket(std::uint32_t rate_per_sec, std::uint32_t burst, Timestamp now = {}) noexcept {
    configure(rate_per_sec, burst, now);
  }

  void configure(std::uint32_t rate_per_sec, std::uint32_t burst, Timestamp now) noexcept {
    rate_ = rate_per_sec;
    capacity_ = static_cast<std::int64_t>(burst == 0 ? rate_per_sec : burst) * kScale;
    tokens_ = capacity_;
    last_ = now;
  }
  [[nodiscard]] bool enabled() const noexcept { return rate_ != 0; }
  FASTMM_FORCE_INLINE void refill(Timestamp now) noexcept {
    if (now <= last_) return;
    const std::int64_t elapsed = (now - last_).ns;
    // rate tokens/s == rate * 1e6 / 1e9 micro-tokens per ns == rate / 1000 per ns
    tokens_ +=
        static_cast<std::int64_t>(static_cast<Int128>(elapsed) * rate_ * kScale / 1'000'000'000);
    if (tokens_ > capacity_) tokens_ = capacity_;
    last_ = now;
  }
  [[nodiscard]] FASTMM_FORCE_INLINE bool try_take(Timestamp now) noexcept {
    if (rate_ == 0) return true;
    refill(now);
    if (tokens_ < kScale) return false;
    tokens_ -= kScale;
    return true;
  }
  // Whole tokens try_take(now) would find, without taking one or refilling (RiskHeadroom).
  // Unlimited (rate 0): INT64_MAX.
  [[nodiscard]] std::int64_t available(Timestamp now) const noexcept {
    if (rate_ == 0) return std::numeric_limits<std::int64_t>::max();
    std::int64_t t = tokens_;
    if (now > last_) {
      t += static_cast<std::int64_t>(static_cast<Int128>((now - last_).ns) * rate_ * kScale /
                                     1'000'000'000);
      if (t > capacity_) t = capacity_;
    }
    return t / kScale;
  }
  // Moves the refill reference to `now` without adding tokens (the engine's start time).
  void rebase(Timestamp now) noexcept { last_ = now; }
  [[nodiscard]] std::int64_t tokens() const noexcept { return tokens_ / kScale; }
  [[nodiscard]] std::int64_t tokens_micro() const noexcept { return tokens_; }

 private:
  std::uint32_t rate_ = 0;
  std::int64_t capacity_ = 0;
  std::int64_t tokens_ = 0;
  Timestamp last_{};
};

struct OrderIntent {
  InstrumentId instrument;
  VenueId venue;
  Side side;
  OrderType type = OrderType::Limit;
  Price price;
  Qty qty;
  TimeInForce tif = TimeInForce::Gtc;  // Ioc / Fok never rest (the feed-lag gate lets them pass)
};

// The order's underlying ([risk.underlying]) as RiskEngine::underlying_inputs measures it, base
// units (raw Qty): the position now and the leaves of the open orders on the order's side, over
// every instrument of the underlying. `limited` false: the order's instrument has no limit to
// check.
struct UnderlyingInputs {
  std::int64_t net = 0;
  std::int64_t open = 0;
  bool limited = false;
  bool known = true;  // false: an inverse contract that counts has no current mark
};

// Dynamic inputs the engine gathers for one check.
struct RiskInputs {
  Timestamp now;
  const Position* position = nullptr;
  // Portfolio exposure before this order (PositionTracker::gross_exposure / net_exposure).
  Notional gross_exposure{};
  Notional net_exposure{};
  Qty open_same_side{};           // leaves of our open orders on the same side
  std::uint32_t open_orders = 0;  // open orders on the instrument
  Price best_own_opposite{};      // best price of our own resting orders on the other side
  bool feed_lagged = false;       // the venue's feed-lag gate holds (VenueHealth::gated)
  UnderlyingInputs underlying{};  // [risk.underlying]; the engine fills it when underlying_on()
  // [risk] check_balance: the account's balance on the venue does not cover the order
  // (BalanceBook::covers); the engine sets it once a venue has reported balances.
  bool balance_short = false;
};

struct RiskStats {
  std::uint64_t checks = 0;
  std::uint64_t passed = 0;
  std::uint64_t rejects[256] = {};
  std::uint64_t trips = 0;
};

class RiskEngine {
 public:
  explicit RiskEngine(const RiskLimits& limits = {}, Timestamp now = {}) noexcept {
    set_limits(limits, now);
  }

  void set_limits(const RiskLimits& l, Timestamp now) noexcept {
    limits_ = l;
    update_flags();
    bucket_.configure(l.orders_per_sec, l.burst, now);
  }
  [[nodiscard]] const RiskLimits& limits() const noexcept { return limits_; }
  [[nodiscard]] const RiskStats& stats() const noexcept { return stats_; }
  [[nodiscard]] TokenBucket& bucket() noexcept { return bucket_; }
  [[nodiscard]] const TokenBucket& bucket() const noexcept { return bucket_; }

  // ---- kill switch (any thread) ---------------------------------------------------------
  void trip() noexcept {
    kill_.fetch_or(1U, std::memory_order_acq_rel);
    ++stats_.trips;
  }
  void trip_venue(VenueId v) noexcept {
    kill_.fetch_or(venue_bit(v), std::memory_order_acq_rel);
    ++stats_.trips;
  }
  void reset() noexcept { kill_.store(0, std::memory_order_release); }
  void reset_venue(VenueId v) noexcept {
    kill_.fetch_and(~venue_bit(v), std::memory_order_acq_rel);
  }
  [[nodiscard]] bool killed() const noexcept {
    return (kill_.load(std::memory_order_acquire) & 1U) != 0;
  }
  [[nodiscard]] bool venue_killed(VenueId v) const noexcept {
    return (kill_.load(std::memory_order_acquire) & venue_bit(v)) != 0;
  }
  // Bit 0 is the global flag; venues 0..30 get bits 1..31 (venues beyond share bit 31).
  [[nodiscard]] static constexpr std::uint32_t venue_bit(VenueId v) noexcept {
    return 1U << (1U + (v.value < 31U ? v.value : 30U));
  }
  // Slot of `v` in per-venue tables sized kKillVenueSlots (the ids that share bit 31 share a slot).
  [[nodiscard]] static constexpr std::size_t venue_slot(VenueId v) noexcept {
    return v.value < kKillVenueSlots ? v.value : kKillVenueSlots - 1;
  }
  [[nodiscard]] std::uint32_t kill_flags() const noexcept {
    return kill_.load(std::memory_order_acquire);
  }
  [[nodiscard]] static constexpr bool cancel_allowed() noexcept { return true; }

  // ---- market state (engine thread) -----------------------------------------------------
  void on_book(InstrumentId id, Price mid, Timestamp ts) noexcept {
    if (FASTMM_UNLIKELY(id.value >= kMaxInstruments)) return;
    auto& m = md_[id.value];
    m.book_ts = ts;
    m.mid = mid;
    m.collar_lo = m.collar_hi = Price{};
    if (limits_.price_collar_bps > 0 && mid.is_positive()) {
      // The mid comes from the book and the width from the configuration: a mid near the
      // fixed-point limit would wrap the upper band and turn the collar into a pass-through.
      const Price band = apply_bps(mid, limits_.price_collar_bps);
      if (const std::optional<Price> hi = checked_add(mid, band); hi) {
        m.collar_lo = mid - band;
        m.collar_hi = *hi;
      }
    }
  }
  void on_trade(InstrumentId id, Price px) noexcept {
    if (FASTMM_UNLIKELY(id.value >= kMaxInstruments)) return;
    auto& m = md_[id.value];
    m.last_trade = px;
    m.ff_lo = m.ff_hi = Price{};
    if (limits_.fat_finger_bps > 0 && px.is_positive()) {
      const Price band = apply_bps(px, limits_.fat_finger_bps);
      if (const std::optional<Price> hi = checked_add(px, band); hi) {
        m.ff_lo = px - band;
        m.ff_hi = *hi;
      }
    }
  }
  // ---- accounting across settlement currencies ([accounting], core/fx.hpp) ------------------
  // With an active plan an order's notional is converted to the reporting currency for the
  // exposure caps, at the mid of its currency's FX source. The rate is usable while the source's
  // book is valid (on_fx_book) and, with the plan's `stale` ([accounting] stale_fx_ms, not
  // [risk] stale_md_ms: a quiet FX book updates seldom) set, no older than that. Without a usable
  // rate an order that adds to exposure in that currency is refused (FxRateUnknown) while
  // max_loss or an exposure cap is set; one that reduces it passes.
  void set_fx(const FxPlan& plan) noexcept {
    fx_on_ = plan.active();
    fx_stale_ = plan.stale;
    update_flags();
    for (std::size_t i = 0; i < kMaxInstruments; ++i) fx_ccy_[i] = plan.ccy[i];
    for (std::size_t c = 0; c < kMaxCurrencies; ++c) {
      fx_src_[c] = plan.sources[c];
      fx_valid_[c] = false;
    }
  }
  // ---- net position per underlying ([risk.underlying], core/underlying.hpp) -------------------
  // Positions and open orders are converted to base units, an inverse contract's at its current
  // mark: the last mid of a valid book, not older than stale_md when that is set. Without one, an
  // order on the underlying is refused (UnderlyingMarkUnknown) while its limit is set; a flatten,
  // which passes no position, is not checked.
  void set_underlying(const UnderlyingPlan& plan) noexcept {
    und_count_ = plan.count;
    for (std::size_t i = 0; i < kMaxInstruments; ++i) und_of_[i] = plan.of[i];
    und_members_ = plan.members;
    und_begin_ = plan.begin;
    for (std::size_t u = 0; u < kMaxUnderlyings; ++u) und_max_[u] = plan.max_net[u].raw;
    update_flags();
  }
  // A new limit for underlying `u` (0 lifts it). Returns false for an index the plan does not have.
  bool set_underlying_limit(std::size_t u, Qty max_net) noexcept {
    if (u >= und_count_ || max_net.raw < 0) return false;
    und_max_[u] = max_net.raw;
    update_flags();
    return true;
  }
  [[nodiscard]] Qty underlying_limit(std::size_t u) const noexcept {
    return Qty::from_raw(u < kMaxUnderlyings ? und_max_[u] : 0);
  }
  // Some underlying has a limit: the engine gathers UnderlyingInputs for its orders.
  [[nodiscard]] bool underlying_on() const noexcept { return und_on_; }
  // The mark an inverse contract is converted at, or zero when it has no current one.
  [[nodiscard]] Price underlying_mark(InstrumentId id, Timestamp now) const noexcept {
    if (id.value >= kMaxInstruments) return {};
    const MdState& md = md_[id.value];
    if (!md.mid.is_positive()) return {};
    if (limits_.stale_md.ns > 0 && (!md.book_ts.valid() || now - md.book_ts > limits_.stale_md))
      return {};
    return md.mid;
  }
  // The net position of underlying `u` in base units (raw); false when an inverse contract with a
  // position has no current mark. `position_of(id)` is the instrument's position in contracts.
  template <class PositionOf>
  bool underlying_net(std::size_t u,
                      const InstrumentTable& insts,
                      Timestamp now,
                      PositionOf&& position_of,
                      std::int64_t& net) const noexcept {
    net = 0;
    bool known = true;
    if (u >= und_count_) return false;
    for (std::size_t k = und_begin_[u]; k < und_begin_[u + 1]; ++k) {
      const InstrumentId j = und_members_[k];
      std::int64_t b = 0;
      if (to_base_units(insts.get(j), position_of(j).raw, underlying_mark(j, now), b)) {
        net += b;
      } else {
        known = false;
      }
    }
    return known;
  }
  // What check_new/check_replace need for an order on `id` of side `side`: the underlying's
  // position now and its open orders on that side, over every instrument of the underlying.
  // `position_of(id)` and `open_of(id)`: an instrument's position and same-side leaves, contracts.
  template <class PositionOf, class OpenOf>
  [[nodiscard]] UnderlyingInputs underlying_inputs(InstrumentId id,
                                                   const InstrumentTable& insts,
                                                   Timestamp now,
                                                   PositionOf&& position_of,
                                                   OpenOf&& open_of) const noexcept {
    UnderlyingInputs r;
    if (id.value >= kMaxInstruments || und_of_[id.value] == 0) return r;
    const std::size_t u = und_of_[id.value] - 1U;
    if (und_max_[u] <= 0) return r;
    r.limited = true;
    for (std::size_t k = und_begin_[u]; k < und_begin_[u + 1]; ++k) {
      const InstrumentId j = und_members_[k];
      const Instrument& inst = insts.get(j);
      const Price mark = underlying_mark(j, now);
      std::int64_t p = 0;
      std::int64_t o = 0;
      if (!to_base_units(inst, position_of(j).raw, mark, p) ||
          !to_base_units(inst, open_of(j).raw, mark, o)) {
        r.known = false;
        return r;
      }
      r.net += p;
      r.open += o;
    }
    return r;
  }

  // The FX source of currency `c` has a valid book now (its mid came through on_book) or not.
  void on_fx_book(std::size_t c, bool valid) noexcept {
    if (c < kMaxCurrencies) fx_valid_[c] = valid;
  }
  // The rate of currency `c` for an order at `now`; unknown when it is not usable.
  [[nodiscard]] FxRate fx_rate(std::size_t c, Timestamp now) const noexcept {
    if (c == 0) return FxRate::identity();
    if (c >= kMaxCurrencies || !fx_valid_[c] || !fx_src_[c].instrument.valid()) return {};
    const MdState& md = md_[fx_src_[c].instrument.value];
    if (fx_stale_.ns > 0 && (!md.book_ts.valid() || now - md.book_ts > fx_stale_)) return {};
    return FxRate::from_mid(md.mid, fx_src_[c].invert);
  }

  // Returns true if the loss limit tripped the kill switch.
  bool on_pnl(Notional net) noexcept {
    if (limits_.max_loss.is_positive() && net.raw <= -limits_.max_loss.raw && !killed()) {
      trip();
      return true;
    }
    return false;
  }

  // ---- headroom (read-only) ------------------------------------------------------------------
  // `buy` and `sell` are the inputs a buy and a sell on `inst` would be checked with; `net_pnl` is
  // what on_pnl is given. The exposure rooms are those of the reporting currency (the engine's
  // totals); an order in another settlement currency adds its notional at that currency's rate.
  [[nodiscard]] RiskHeadroom headroom(const Instrument& inst,
                                      const RiskInputs& buy,
                                      const RiskInputs& sell,
                                      Notional net_pnl) const noexcept {
    RiskHeadroom h;
    h.order_tokens = bucket_.available(buy.now);
    if (limits_.max_open_orders > 0) {
      const std::int64_t left = static_cast<std::int64_t>(limits_.max_open_orders) -
                                static_cast<std::int64_t>(buy.open_orders);
      h.open_orders = left > 0 ? left : 0;
    }
    if (limits_.max_order_qty.is_positive()) h.max_order_qty = limits_.max_order_qty;
    if (limits_.max_order_notional.is_positive()) h.max_order_notional = limits_.max_order_notional;
    if (limits_.max_position.is_positive() && buy.position != nullptr) {
      const std::int64_t m = limits_.max_position.raw;
      const std::int64_t q = buy.position->qty.raw;
      // Buy: predicted = q + open + x passes while <= m, or while |predicted| <= |q| (q < 0).
      std::int64_t b = m - q - buy.open_same_side.raw;
      if (q < 0) b = std::max(b, -2 * q - buy.open_same_side.raw);
      std::int64_t s = m + q - sell.open_same_side.raw;
      if (q > 0) s = std::max(s, 2 * q - sell.open_same_side.raw);
      h.buy_qty = lot_floor(inst, b);
      h.sell_qty = lot_floor(inst, s);
    }
    if (limits_.max_gross_notional.is_positive()) {
      const std::int64_t g = limits_.max_gross_notional.raw - buy.gross_exposure.raw;
      h.gross_notional = Notional::from_raw(g > 0 ? g : 0);
    }
    if (limits_.max_net_notional.is_positive()) {
      // net + x passes while |net + x| <= max, or while it does not grow |net|.
      const std::int64_t m = limits_.max_net_notional.raw;
      const std::int64_t n = buy.net_exposure.raw;
      std::int64_t b = m - n;
      if (n < 0) b = std::max(b, -2 * n);
      std::int64_t s = m + n;
      if (n > 0) s = std::max(s, 2 * n);
      h.net_buy_notional = Notional::from_raw(b > 0 ? b : 0);
      h.net_sell_notional = Notional::from_raw(s > 0 ? s : 0);
    }
    if (portfolio_ && buy.position != nullptr && inst.id.value < kMaxInstruments) {
      const std::int64_t q = buy.position->qty.raw;
      const std::uint8_t c = fx_ccy_[inst.id.value];
      const auto room = [&](bool is_buy, Notional net) {
        if (q != 0 && (q > 0) != is_buy) return Notional::max();  // reduces: never refused
        Notional r = std::min(h.gross_notional, net);
        if (!fx_gate_ || c == 0) return r;
        const FxRate rate = fx_rate(c, buy.now);
        if (!rate.known()) return Notional{};
        if (r == Notional::max() || rate.num == rate.den) return r;
        // Rounded down, so that converting it back stays inside the room.
        const Int128 n = static_cast<Int128>(r.raw) * rate.den / rate.num;
        return n >= Notional::max().raw ? Notional::max()
                                        : Notional::from_raw(static_cast<std::int64_t>(n));
      };
      h.exposure_buy_notional = room(true, h.net_buy_notional);
      h.exposure_sell_notional = room(false, h.net_sell_notional);
    }
    if (limits_.max_loss.is_positive())
      h.loss_budget = Notional::from_raw(limits_.max_loss.raw + net_pnl.raw);
    if (buy.underlying.limited && inst.id.value < kMaxInstruments) {
      const std::int64_t m = und_max_[und_of_[inst.id.value] - 1U];
      const Price mark = underlying_mark(inst.id, buy.now);
      // The same arithmetic as max_position, in base units, then back to contracts.
      const auto room = [&](const UnderlyingInputs& u, bool is_buy) {
        if (!u.known || (inst.inverse() && !mark.is_positive())) return Qty{};
        const std::int64_t q = is_buy ? u.net : -u.net;
        std::int64_t r = m - q - u.open;
        if (q < 0) r = std::max(r, -2 * q - u.open);
        if (r <= 0) return Qty{};
        const Int128 c = inst.inverse()
                             ? static_cast<Int128>(r) * mark.raw / inst.contract_multiplier.raw
                             : static_cast<Int128>(r) * kFixedScale / inst.contract_multiplier.raw;
        return lot_floor(inst, static_cast<std::int64_t>(c));
      };
      h.underlying_buy_qty = room(buy.underlying, true);
      h.underlying_sell_qty = room(sell.underlying, false);
    }
    return h;
  }

  // ---- checks ---------------------------------------------------------------------------
  // Order of evaluation is the RejectReason enum order 1..15.
  [[nodiscard]] RejectReason check_new(const OrderIntent& o,
                                       const Instrument& inst,
                                       const RiskInputs& in) noexcept {
    return check(o, inst, in, Qty{}, true);
  }
  // Replace: the existing order's leaves are excluded from the predicted position and the
  // open-order cap does not apply (the count is unchanged).
  [[nodiscard]] RejectReason check_replace(const OrderIntent& o,
                                           const Order& existing,
                                           const Instrument& inst,
                                           const RiskInputs& in) noexcept {
    return check(o, inst, in, existing.leaves_qty(), false);
  }

 private:
  struct MdState {
    Timestamp book_ts{};
    Price mid{};
    Price last_trade{};
    Price collar_lo{};
    Price collar_hi{};
    Price ff_lo{};
    Price ff_hi{};
  };

  RejectReason check(const OrderIntent& o,
                     const Instrument& inst,
                     const RiskInputs& in,
                     Qty exclude_open,
                     bool count_order) noexcept {
    ++stats_.checks;
    const RejectReason r = evaluate(o, inst, in, exclude_open, count_order);
    if (r == RejectReason::None) {
      ++stats_.passed;
    } else {
      ++stats_.rejects[static_cast<std::uint8_t>(r)];
    }
    return r;
  }

  RejectReason evaluate(const OrderIntent& o,
                        const Instrument& inst,
                        const RiskInputs& in,
                        Qty exclude_open,
                        bool count_order) noexcept {
    const std::uint32_t kill = kill_.load(std::memory_order_acquire);
    if (FASTMM_UNLIKELY((kill & 1U) != 0)) return RejectReason::KillSwitch;
    if (FASTMM_UNLIKELY((kill & venue_bit(o.venue)) != 0)) return RejectReason::VenueKilled;
    if (FASTMM_UNLIKELY(!inst.enabled() || o.instrument.value >= kMaxInstruments))
      return RejectReason::InstrumentDisabled;
    if (o.type != OrderType::Market && !inst.valid_price(o.price)) return RejectReason::InvalidTick;
    if (!inst.valid_qty(o.qty)) return RejectReason::InvalidLot;
    const Notional notional =
        inst.notional(o.type == OrderType::Market ? md_[o.instrument.value].mid : o.price, o.qty);
    if (inst.min_notional.is_positive() && notional < inst.min_notional)
      return RejectReason::BelowMinNotional;
    const MdState& md = md_[o.instrument.value];
    if (limits_.stale_md.ns > 0 &&
        (!md.book_ts.valid() || in.now - md.book_ts > limits_.stale_md)) {
      return RejectReason::StaleMarketData;
    }
    if (FASTMM_UNLIKELY(in.feed_lagged) && may_rest(o) && !reduces_position(o, in))
      return RejectReason::FeedLag;
    if (o.type != OrderType::Market && md.collar_hi.is_positive() &&
        (o.price < md.collar_lo || o.price > md.collar_hi)) {
      return RejectReason::PriceCollar;
    }
    if (o.type != OrderType::Market && md.ff_hi.is_positive() &&
        (o.price < md.ff_lo || o.price > md.ff_hi)) {
      return RejectReason::FatFinger;
    }
    if (limits_.max_order_qty.is_positive() && o.qty > limits_.max_order_qty)
      return RejectReason::MaxOrderQty;
    if (limits_.max_order_notional.is_positive() && notional > limits_.max_order_notional) {
      return RejectReason::MaxOrderNotional;
    }
    if (limits_.max_position.is_positive() && in.position != nullptr) {
      // predicted = position + (same-side open - excluded) + this order, in the order's direction
      const std::int64_t dir = sign(o.side);
      const std::int64_t predicted =
          in.position->qty.raw + dir * (in.open_same_side.raw - exclude_open.raw + o.qty.raw);
      const std::int64_t abs_pred = predicted < 0 ? -predicted : predicted;
      // only reject if the order increases exposure beyond the cap
      const std::int64_t cur_abs =
          in.position->qty.raw < 0 ? -in.position->qty.raw : in.position->qty.raw;
      if (abs_pred > limits_.max_position.raw && abs_pred > cur_abs)
        return RejectReason::MaxPosition;
    }
    // Portfolio exposure: what this order would add on top of what is already marked. Like
    // max_position, an order that reduces exposure is never refused by it. One flag covers the
    // caps and the FX gate, so a session with neither pays one branch.
    if (FASTMM_UNLIKELY(portfolio_) && in.position != nullptr) {
      // In another settlement currency than the reporting one, the exposure this order adds is
      // measured at its currency's rate, and without a usable rate it is not added at all.
      Notional exposure = notional;
      const std::uint8_t c = fx_ccy_[o.instrument.value];
      const std::int64_t q = in.position->qty.raw;
      if (fx_gate_ && c != 0 && (q == 0 || (q > 0) == (o.side == Side::Buy))) {
        const FxRate rate = fx_rate(c, in.now);
        if (!rate.known()) return RejectReason::FxRateUnknown;
        exposure = convert(notional, rate);
      }
      const std::int64_t dir = sign(o.side);
      const bool reduces = in.position->qty.raw != 0 && (in.position->qty.raw > 0) != (dir > 0);
      if (!reduces) {
        if (limits_.max_gross_notional.is_positive() &&
            in.gross_exposure + exposure > limits_.max_gross_notional) {
          return RejectReason::MaxGrossNotional;
        }
        if (limits_.max_net_notional.is_positive()) {
          // The net can be over the cap already (a mark moved, or a limit was tightened); an order
          // that brings it towards zero is how you get back under it.
          const Notional net = in.net_exposure + (dir > 0 ? exposure : Notional{} - exposure);
          if (net.abs() > limits_.max_net_notional && net.abs() > in.net_exposure.abs())
            return RejectReason::MaxNetNotional;
        }
      }
    }
    // Net position per underlying: worst case in the order's direction, as max_position, over every
    // instrument of the underlying. The engine fills the inputs only for an order on a limited
    // underlying: without one this is one branch.
    if (FASTMM_UNLIKELY(in.underlying.limited) && in.position != nullptr) {
      if (!in.underlying.known) return RejectReason::UnderlyingMarkUnknown;
      std::int64_t add = 0;
      if (!to_base_units(
              inst, o.qty.raw - exclude_open.raw, underlying_mark(o.instrument, in.now), add))
        return RejectReason::UnderlyingMarkUnknown;
      const std::int64_t dir = sign(o.side);
      const std::int64_t worst = in.underlying.net + dir * (in.underlying.open + add);
      if (underlying_exceeds(und_max_[und_of_[o.instrument.value] - 1U], in.underlying.net, worst))
        return RejectReason::MaxUnderlyingNet;
    }
    if (count_order && limits_.max_open_orders > 0 && in.open_orders >= limits_.max_open_orders) {
      return RejectReason::MaxOpenOrders;
    }
    if (limits_.stp && o.type != OrderType::Market && in.best_own_opposite.is_positive() &&
        at_or_better_cross(o.side, o.price, in.best_own_opposite)) {
      return RejectReason::SelfTradePrevention;
    }
    if (FASTMM_UNLIKELY(in.balance_short)) return RejectReason::BalanceShort;
    if (!bucket_.try_take(in.now)) return RejectReason::RateLimit;
    return RejectReason::None;
  }
  // A limit order that is neither IOC nor FOK can rest at the venue.
  static constexpr bool may_rest(const OrderIntent& o) noexcept {
    return o.type != OrderType::Market && o.tif != TimeInForce::Ioc && o.tif != TimeInForce::Fok;
  }
  // The order, with our open orders on its side, only takes the position towards zero.
  static constexpr bool reduces_position(const OrderIntent& o, const RiskInputs& in) noexcept {
    if (in.position == nullptr) return false;
    const std::int64_t q = in.position->qty.raw;
    if (q == 0 || (q > 0) == (o.side == Side::Buy)) return false;
    return in.open_same_side.raw + o.qty.raw <= (q < 0 ? -q : q);
  }
  void update_flags() noexcept {
    fx_gate_ = fx_on_ && limits_.reads_totals();
    portfolio_ = fx_gate_ || limits_.max_gross_notional.is_positive() ||
                 limits_.max_net_notional.is_positive();
    und_on_ = false;
    for (std::size_t u = 0; u < und_count_; ++u) und_on_ = und_on_ || und_max_[u] > 0;
  }
  // Would a `side` order at `px` trade against our own resting order at `own_opposite`?
  static constexpr bool at_or_better_cross(Side side, Price px, Price own_opposite) noexcept {
    return side == Side::Buy ? px >= own_opposite : px <= own_opposite;
  }

  static Qty lot_floor(const Instrument& inst, std::int64_t raw) noexcept {
    if (raw <= 0) return Qty{};
    if (!inst.lot.is_positive()) return Qty::from_raw(raw);
    return Qty::from_raw(raw - raw % inst.lot.raw);
  }

  RiskLimits limits_{};
  TokenBucket bucket_{};
  RiskStats stats_{};
  alignas(kCacheLine) std::atomic<std::uint32_t> kill_{0};
  MdState md_[kMaxInstruments] = {};
  // [accounting], after md_: its layout is the hot path's.
  bool portfolio_ = false;  // an exposure cap or fx_gate_: the portfolio block runs
  bool fx_gate_ = false;    // fx_on_ and a limit that reads the totals
  bool fx_on_ = false;
  Duration fx_stale_{};  // [accounting] stale_fx_ms
  bool und_on_ = false;  // [risk.underlying]: some underlying has a limit
  std::array<bool, kMaxCurrencies> fx_valid_{};
  std::array<FxSource, kMaxCurrencies> fx_src_{};
  std::array<std::uint8_t, kMaxInstruments> fx_ccy_{};
  // [risk.underlying], after the rest for the same reason (und_on_ sits with the flags above).
  std::uint8_t und_count_ = 0;
  std::array<std::int64_t, kMaxUnderlyings> und_max_{};  // raw base units; 0: no limit
  std::array<std::uint8_t, kMaxInstruments> und_of_{};   // 1 + underlying index; 0: none
  std::array<InstrumentId, kMaxInstruments> und_members_{};
  std::array<std::uint16_t, kMaxUnderlyings + 1> und_begin_{};
};

}  // namespace fastmm
