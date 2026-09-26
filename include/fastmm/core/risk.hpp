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

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
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
  // book is valid (on_fx_book) and, with stale_md set, no older than stale_md. Without a usable
  // rate an order that adds to exposure in that currency is refused (FxRateUnknown) while
  // max_loss or an exposure cap is set; one that reduces it passes.
  void set_fx(const FxPlan& plan) noexcept {
    fx_on_ = plan.active();
    update_flags();
    for (std::size_t i = 0; i < kMaxInstruments; ++i) fx_ccy_[i] = plan.ccy[i];
    for (std::size_t c = 0; c < kMaxCurrencies; ++c) {
      fx_src_[c] = plan.sources[c];
      fx_valid_[c] = false;
    }
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
    if (limits_.stale_md.ns > 0 && (!md.book_ts.valid() || now - md.book_ts > limits_.stale_md))
      return {};
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
    if (count_order && limits_.max_open_orders > 0 && in.open_orders >= limits_.max_open_orders) {
      return RejectReason::MaxOpenOrders;
    }
    if (limits_.stp && o.type != OrderType::Market && in.best_own_opposite.is_positive() &&
        at_or_better_cross(o.side, o.price, in.best_own_opposite)) {
      return RejectReason::SelfTradePrevention;
    }
    if (!bucket_.try_take(in.now)) return RejectReason::RateLimit;
    return RejectReason::None;
  }
  void update_flags() noexcept {
    fx_gate_ = fx_on_ && limits_.reads_totals();
    portfolio_ = fx_gate_ || limits_.max_gross_notional.is_positive() ||
                 limits_.max_net_notional.is_positive();
  }
  // Would a `side` order at `px` trade against our own resting order at `own_opposite`?
  static constexpr bool at_or_better_cross(Side side, Price px, Price own_opposite) noexcept {
    return side == Side::Buy ? px >= own_opposite : px <= own_opposite;
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
  std::array<bool, kMaxCurrencies> fx_valid_{};
  std::array<FxSource, kMaxCurrencies> fx_src_{};
  std::array<std::uint8_t, kMaxInstruments> fx_ccy_{};
};

}  // namespace fastmm
