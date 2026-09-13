#include "fastmm/sim/market_generator.hpp"

#include <cmath>

namespace fastmm::sim {

namespace {
constexpr double kTwoPi = 6.283185307179586;
}

MarketGenerator::MarketGenerator(const MarketGeneratorParams& p,
                                 std::uint64_t seed,
                                 InstrumentId instrument,
                                 Timestamp start,
                                 Timestamp end)
    : p_(p),
      rng_(seed),
      inst_(instrument),
      end_(end),
      mid_(round_to_tick_nearest(p.start_mid, p.tick)) {
  resting_.reserve(p_.max_resting * 2 + 16);
  next_limit_ = start + exp_delay(p_.limit_rate_per_s);
  next_cancel_ = start + exp_delay(p_.cancel_rate_per_order_s);
  next_market_ = start + exp_delay(p_.market_rate_per_s);
  next_mid_ = start + exp_delay(p_.mid_step_rate_per_s);
  next_regime_ = p_.regimes ? start + exp_delay(p_.regime_switch_rate_per_s) : Timestamp::max();
}

Duration MarketGenerator::exp_delay(double rate_per_s) noexcept {
  if (rate_per_s <= 0.0) return Duration{INT64_MAX / 4};
  const double u = 1.0 - rng_.uniform01();  // (0, 1]
  const double s = -std::log(u) / rate_per_s;
  return Duration{static_cast<std::int64_t>(s * 1e9) + 1};
}

Qty MarketGenerator::lognormal_qty(double median_lots, double sigma) noexcept {
  const double u1 = 1.0 - rng_.uniform01();
  const double u2 = rng_.uniform01();
  const double z = std::sqrt(-2.0 * std::log(u1)) * std::cos(kTwoPi * u2);
  double lots = median_lots * std::exp(sigma * z);
  if (lots < 1.0) lots = 1.0;
  if (lots > 1e9) lots = 1e9;
  return Qty::from_raw(static_cast<std::int64_t>(lots) * p_.lot.raw);
}

Timestamp MarketGenerator::next_ts() const noexcept {
  Timestamp t = next_limit_;
  if (next_cancel_ < t) t = next_cancel_;
  if (next_market_ < t) t = next_market_;
  if (next_mid_ < t) t = next_mid_;
  if (next_regime_ < t) t = next_regime_;
  return t > end_ ? Timestamp::max() : t;
}

void MarketGenerator::seed_book(MatchingEngine& me, int levels, Timestamp now) noexcept {
  for (int k = 0; k < levels; ++k) {
    for (Side side : {Side::Buy, Side::Sell}) {
      const std::int64_t off = (p_.base_spread_ticks + k) * p_.tick.raw;
      NewOrder o;
      o.account = kGeneratorAccount;
      o.cl_ord_id = ClientOrderId{next_id_++};
      o.instrument = inst_;
      o.side = side;
      o.type = OrderType::Limit;
      o.tif = TimeInForce::Gtc;
      o.price = Price::from_raw(side == Side::Buy ? mid_.raw - off : mid_.raw + off);
      o.qty = lognormal_qty(p_.limit_qty_median_lots, p_.limit_qty_sigma);
      if (me.submit(o, now).accepted()) {
        resting_.push_back(o.cl_ord_id);
        ++stats_.limits;
      }
    }
  }
}

void MarketGenerator::step(MatchingEngine& me) noexcept {
  const Timestamp now = next_ts();
  if (now == Timestamp::max()) return;
  if (now == next_limit_) {
    do_limit(me, now);
    next_limit_ = now + exp_delay(p_.limit_rate_per_s);
  } else if (now == next_cancel_) {
    do_cancel(me, now);
    reschedule_cancel(now);
  } else if (now == next_market_) {
    do_market(me, now);
    next_market_ = now + exp_delay(p_.market_rate_per_s * regime_mult());
  } else if (now == next_mid_) {
    mid_ = Price::from_raw(mid_.raw + (rng_.uniform(2) == 0 ? -p_.tick.raw : p_.tick.raw));
    if (mid_.raw < 2 * p_.tick.raw) mid_ = Price::from_raw(2 * p_.tick.raw);
    ++stats_.mid_steps;
    next_mid_ = now + exp_delay(p_.mid_step_rate_per_s * regime_mult());
  } else {
    volatile_ = !volatile_;
    ++stats_.regime_switches;
    next_regime_ = now + exp_delay(p_.regime_switch_rate_per_s);
  }
}

void MarketGenerator::do_limit(MatchingEngine& me, Timestamp now) noexcept {
  if (resting_.size() >= resting_.capacity() - 1) compact(me);
  if (resting_.size() >= p_.max_resting) return;  // book saturated: skip this arrival
  const Side side = rng_.uniform(2) == 0 ? Side::Buy : Side::Sell;
  // geometric offset in ticks: number of failures before the first success
  std::int64_t k = 0;
  while (rng_.uniform01() >= p_.offset_p && k < 200) ++k;
  const std::int64_t off = (p_.base_spread_ticks + k) * p_.tick.raw;
  NewOrder o;
  o.account = kGeneratorAccount;
  o.cl_ord_id = ClientOrderId{next_id_++};
  o.instrument = inst_;
  o.side = side;
  o.type = OrderType::Limit;
  o.tif = TimeInForce::Gtc;
  o.price = Price::from_raw(side == Side::Buy ? mid_.raw - off : mid_.raw + off);
  if (!o.price.is_positive()) o.price = p_.tick;
  o.qty = lognormal_qty(p_.limit_qty_median_lots, p_.limit_qty_sigma);
  const SubmitResult r = me.submit(o, now);
  if (!r.accepted()) {
    ++stats_.rejected;
    return;
  }
  ++stats_.limits;
  if (r.resting.is_positive()) resting_.push_back(o.cl_ord_id);
}

void MarketGenerator::do_cancel(MatchingEngine& me, Timestamp now) noexcept {
  for (int attempt = 0; attempt < 8 && !resting_.empty(); ++attempt) {
    const std::size_t k = rng_.uniform(resting_.size());
    const ClientOrderId id = resting_[k];
    resting_[k] = resting_.back();
    resting_.pop_back();
    if (me.find(kGeneratorAccount, id) == nullptr) continue;  // already filled
    me.cancel(kGeneratorAccount, id, now);
    ++stats_.cancels;
    return;
  }
}

void MarketGenerator::reschedule_cancel(Timestamp now) noexcept {
  const double n = static_cast<double>(resting_.empty() ? 1 : resting_.size());
  next_cancel_ = now + exp_delay(p_.cancel_rate_per_order_s * n);
}

void MarketGenerator::do_market(MatchingEngine& me, Timestamp now) noexcept {
  NewOrder o;
  o.account = kGeneratorAccount;
  o.cl_ord_id = ClientOrderId{next_id_++};
  o.instrument = inst_;
  o.side = rng_.uniform(2) == 0 ? Side::Buy : Side::Sell;
  o.type = OrderType::Market;
  o.tif = TimeInForce::Ioc;
  o.qty = lognormal_qty(p_.market_qty_median_lots, p_.market_qty_sigma);
  if (me.submit(o, now).accepted()) {
    ++stats_.markets;
  } else {
    ++stats_.rejected;
  }
}

void MarketGenerator::compact(const MatchingEngine& me) noexcept {
  std::size_t w = 0;
  for (std::size_t i = 0; i < resting_.size(); ++i) {
    if (me.find(kGeneratorAccount, resting_[i]) != nullptr) resting_[w++] = resting_[i];
  }
  resting_.resize(w);
}

}  // namespace fastmm::sim
