#include "fastmm/backtest/metrics.hpp"

#include "fastmm/backtest/result.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/latency.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace fastmm::bt {

namespace {
double dec(std::int64_t raw) noexcept {
  return static_cast<double>(raw) / static_cast<double>(kFixedScale);
}
double bps_of(std::int64_t amount, std::int64_t notional) noexcept {
  return notional == 0 ? 0.0 : dec(amount) / dec(notional) * 1e4;
}
// Percentile of a sorted vector by nearest rank; 0 when empty.
double percentile_of(const std::vector<std::int64_t>& sorted, double p) noexcept {
  if (sorted.empty()) return 0.0;
  const auto n = static_cast<double>(sorted.size());
  auto i = static_cast<std::size_t>(p * n);
  if (i >= sorted.size()) i = sorted.size() - 1;
  return dec(sorted[i]);
}
}  // namespace

double MarkoutBucket::markout_quote() const noexcept {
  return dec(markout_raw);
}
double MarkoutBucket::capture_quote() const noexcept {
  return dec(capture_raw);
}
double MarkoutBucket::adverse_selection_quote() const noexcept {
  return dec(capture_raw - markout_raw);
}
double MarkoutBucket::notional() const noexcept {
  return dec(notional_raw);
}
double MarkoutBucket::markout_bps() const noexcept {
  return bps_of(markout_raw, notional_raw);
}
double MarkoutBucket::capture_bps() const noexcept {
  return bps_of(capture_raw, notional_raw);
}
double MarkoutBucket::adverse_selection_bps() const noexcept {
  return bps_of(capture_raw - markout_raw, notional_raw);
}

std::string MarkoutHorizon::label() const {
  const std::int64_t ns = horizon_ns;
  if (ns % 60'000'000'000LL == 0) return fmt::format("{}m", ns / 60'000'000'000LL);
  if (ns % 1'000'000'000LL == 0) return fmt::format("{}s", ns / 1'000'000'000LL);
  if (ns % 1'000'000LL == 0) return fmt::format("{}ms", ns / 1'000'000LL);
  return fmt::format("{}us", ns / 1'000LL);
}

const MarkoutHorizon* Metrics::markout(std::int64_t horizon_ns) const noexcept {
  for (const MarkoutHorizon& h : markouts) {
    if (h.horizon_ns == horizon_ns) return &h;
  }
  return nullptr;
}

Metrics compute_metrics(const EquityRows& equity,
                        const FillRows& fills,
                        const OrderRows& orders,
                        const MetricsInputs& in) {
  Metrics m;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  m.max_drawdown_pct = in.initial_capital > 0.0 ? 0.0 : nan;
  m.rejects = in.rejects;
  m.wall_tick_to_order_p50_ns = in.wall_p50_ns;
  m.wall_tick_to_order_p99_ns = in.wall_p99_ns;

  // ---- equity bars ------------------------------------------------------------------------
  const std::size_t n = equity.size();
  m.bars = n;
  if (n > 0) {
    m.realized_pnl = dec(equity.realized[n - 1]);
    m.unrealized_pnl = dec(equity.unrealized[n - 1]);
    m.fees = dec(equity.fees[n - 1]);
    m.net_pnl = dec(equity.equity(n - 1));
    m.final_position = dec(equity.position[n - 1]);
    m.duration_s = static_cast<double>(equity.ts[n - 1] - equity.ts[0] + in.bar.ns) / 1e9;
    double sum = 0.0;
    double sum2 = 0.0;
    std::int64_t prev = 0;  // equity before the first bar is zero
    std::int64_t peak = 0;
    std::int64_t max_dd = 0;
    double inv_sum = 0.0;
    double inv_abs = 0.0;
    double inv_max = 0.0;
    std::uint64_t both = 0;
    for (std::size_t i = 0; i < n; ++i) {
      const std::int64_t e = equity.equity(i);
      const double r = dec(e - prev);
      sum += r;
      sum2 += r * r;
      prev = e;
      if (e > peak) peak = e;
      if (peak - e > max_dd) max_dd = peak - e;
      const double pos = dec(equity.position[i]);
      inv_sum += pos;
      inv_abs += std::fabs(pos);
      if (std::fabs(pos) > inv_max) inv_max = std::fabs(pos);
      if ((equity.quoted[i] & 3U) == 3U) ++both;
    }
    const double dn = static_cast<double>(n);
    const double mean = sum / dn;
    const double var = n > 1 ? (sum2 - dn * mean * mean) / (dn - 1.0) : 0.0;
    const double sd = var > 0.0 ? std::sqrt(var) : 0.0;
    m.sharpe_bar = sd > 0.0 ? mean / sd : 0.0;
    const double bars_per_year = 365.0 * 86400.0 * 1e9 / static_cast<double>(in.bar.ns);
    m.sharpe_annualized =
        m.duration_s >= kMinAnnualizedDurationS ? m.sharpe_bar * std::sqrt(bars_per_year) : nan;
    m.max_drawdown = dec(max_dd);
    if (in.initial_capital > 0.0) m.max_drawdown_pct = m.max_drawdown / in.initial_capital;
    m.inventory_mean = inv_sum / dn;
    m.inventory_abs_mean = inv_abs / dn;
    m.inventory_max = inv_max;
    m.quote_uptime = static_cast<double>(both) / dn;
  }

  // ---- fills: volume, spread capture, fill quality, PnL decomposition -----------------------
  m.fills = fills.size();
  const std::size_t horizons = fills.markout_horizon_ns.size();
  m.markouts.resize(horizons);
  std::uint32_t max_instrument = 0;
  for (std::uint32_t id : fills.instrument) max_instrument = std::max(max_instrument, id);
  for (std::size_t j = 0; j < horizons; ++j) {
    m.markouts[j].horizon_ns = fills.markout_horizon_ns[j];
    m.markouts[j].instrument.resize(fills.size() == 0 ? 0 : max_instrument + 1);
  }

  double spread_sum = 0.0;
  std::uint64_t spread_n = 0;
  std::int64_t capture_raw = 0;           // signed_qty * (mid at fill - price)
  std::int64_t drift_raw = 0;             // signed_qty * (final mid - mid at fill)
  std::int64_t notional_raw = 0;          // |qty| * price, every fill
  std::int64_t capture_notional_raw = 0;  // the fills that had a venue mid at the fill
  std::int64_t fees_paid_raw = 0;
  std::int64_t rebates_raw = 0;
  std::uint64_t at_touch = 0;
  std::uint64_t behind_touch = 0;
  std::uint64_t through_touch = 0;
  std::vector<std::int64_t> queue_ahead;
  // cl_ord_id -> the fill's own earliest timestamp, for the time-to-fill distribution.
  std::unordered_map<std::uint64_t, std::int64_t> first_fill_ts;

  for (std::size_t i = 0; i < fills.size(); ++i) {
    const bool taker = fills.liquidity[i] == static_cast<std::uint8_t>(Liquidity::Taker);
    if (taker) {
      ++m.taker_fills;
    } else {
      ++m.maker_fills;
    }
    const double q = dec(fills.qty[i]);
    const double px_f = dec(fills.price[i]);
    m.volume_base += q;
    m.volume_quote += q * px_f;

    const bool buy = fills.side[i] == 0;
    const Qty qty = Qty::from_raw(fills.qty[i]);
    const Price price = Price::from_raw(fills.price[i]);
    const Price mid0 = Price::from_raw(fills.mid[i]);
    const std::int64_t sign = buy ? 1 : -1;
    notional_raw += mul(price, qty).raw;
    const std::int64_t fee = fills.fee[i];
    (fee >= 0 ? fees_paid_raw : rebates_raw) += fee >= 0 ? fee : -fee;

    first_fill_ts.try_emplace(fills.cl_ord_id[i], fills.ts[i]);  // fills are in time order

    if (fills.queue_ahead.size() == fills.size() && fills.queue_ahead[i] >= 0)
      queue_ahead.push_back(fills.queue_ahead[i]);

    // At the touch / behind it / through it, against the venue book at the fill.
    if (fills.best_bid.size() == fills.size() && fills.best_ask.size() == fills.size()) {
      const std::int64_t own = buy ? fills.best_bid[i] : fills.best_ask[i];
      const std::int64_t opp = buy ? fills.best_ask[i] : fills.best_bid[i];
      const bool crossed = opp > 0 && (buy ? fills.price[i] >= opp : fills.price[i] <= opp);
      if (crossed) {
        ++through_touch;
      } else if (own > 0 && fills.price[i] == own) {
        ++at_touch;
      } else if (own > 0) {
        ++behind_touch;
      }
    }

    const auto exclude = [&](MarkoutHorizon& h) {
      ++h.excluded_fills;
      if (in.end_ts > 0 && fills.ts[i] + h.horizon_ns > in.end_ts) {
        ++h.excluded_past_end;
      } else {
        ++h.excluded_no_mid;
      }
    };
    const std::uint32_t inst = fills.instrument[i];
    const bool have_final_mid = inst < in.final_mid.size() && in.final_mid[inst] > 0;
    if (!mid0.is_positive()) {
      // No mid at the fill, so nothing can be called spread capture: the whole move from the
      // fill price to the final mid is booked as drift and the identity still holds.
      if (have_final_mid)
        drift_raw += sign * mul(Price::from_raw(in.final_mid[inst]) - price, qty).raw;
      for (std::size_t j = 0; j < horizons; ++j) exclude(m.markouts[j]);
      continue;
    }
    const std::int64_t cap = sign * mul(mid0 - price, qty).raw;
    capture_raw += cap;
    capture_notional_raw += mul(price, qty).raw;
    const double mid_f = dec(fills.mid[i]);
    spread_sum += (buy ? mid_f - px_f : px_f - mid_f) / mid_f * 1e4;
    ++spread_n;

    if (have_final_mid) {
      drift_raw += sign * mul(Price::from_raw(in.final_mid[inst]) - mid0, qty).raw;
    }

    for (std::size_t j = 0; j < horizons; ++j) {
      MarkoutHorizon& h = m.markouts[j];
      const std::int64_t mid_h = fills.markout_mid[j][i];
      if (mid_h <= 0) {
        exclude(h);
        continue;
      }
      const std::int64_t mo = sign * mul(Price::from_raw(mid_h) - price, qty).raw;
      const std::int64_t notional = mul(price, qty).raw;
      h.total.add(mo, cap, notional);
      (buy ? h.buy : h.sell).add(mo, cap, notional);
      (taker ? h.taker : h.maker).add(mo, cap, notional);
      if (inst < h.instrument.size()) h.instrument[inst].add(mo, cap, notional);
    }
  }
  m.spread_captured_bps = spread_n > 0 ? spread_sum / static_cast<double>(spread_n) : 0.0;

  PnlDecomposition& d = m.decomposition;
  d.spread_capture = dec(capture_raw);
  d.spread_capture_bps = bps_of(capture_raw, capture_notional_raw);
  d.mid_drift = dec(drift_raw);
  d.fees_paid = dec(fees_paid_raw);
  d.rebates_received = dec(rebates_raw);
  d.notional = dec(notional_raw);
  d.capture_notional = dec(capture_notional_raw);
  d.capture_fills = spread_n;
  d.fills = m.fills;
  d.net = d.spread_capture + d.mid_drift - d.fees_paid + d.rebates_received;
  d.residual = m.net_pnl - d.net;

  FillQuality& fq = m.fill_quality;
  fq.realized_spread_quote = dec(capture_raw);
  fq.realized_spread_bps = bps_of(capture_raw, capture_notional_raw);
  if (m.fills > 0) {
    const auto dn = static_cast<double>(m.fills);
    fq.at_touch_share = static_cast<double>(at_touch) / dn;
    fq.behind_touch_share = static_cast<double>(behind_touch) / dn;
    fq.through_touch_share = static_cast<double>(through_touch) / dn;
  }
  fq.queue_position_known = in.queue_position_known && !queue_ahead.empty();
  if (!queue_ahead.empty()) {
    std::sort(queue_ahead.begin(), queue_ahead.end());
    double sum = 0.0;
    for (std::int64_t v : queue_ahead) sum += dec(v);
    fq.queue_ahead_mean = sum / static_cast<double>(queue_ahead.size());
    fq.queue_ahead_p50 = percentile_of(queue_ahead, 0.5);
    fq.queue_ahead_p90 = percentile_of(queue_ahead, 0.9);
  }

  // ---- orders -----------------------------------------------------------------------------
  LogLinearHistogram virt;
  std::vector<std::int64_t> ttf_sorted;
  for (std::size_t i = 0; i < orders.size(); ++i) {
    switch (orders.kind[i]) {
      case kOrderKindNew:
        ++m.orders;
        break;
      case kOrderKindCancel:
        ++m.cancels;
        break;
      case kOrderKindReplace:
        ++m.replaces;
        break;
      default:
        break;
    }
    if (orders.venue_ts[i] > 0 && orders.trigger_ts[i] > 0 &&
        orders.venue_ts[i] >= orders.trigger_ts[i]) {
      virt.record(static_cast<std::uint64_t>(orders.venue_ts[i] - orders.trigger_ts[i]));
    }
    if (orders.kind[i] != kOrderKindNew || orders.venue_ts[i] <= 0) continue;
    const auto it = first_fill_ts.find(orders.cl_ord_id[i]);
    if (it == first_fill_ts.end()) continue;
    ++fq.quotes_filled;
    if (it->second >= orders.venue_ts[i]) ttf_sorted.push_back(it->second - orders.venue_ts[i]);
  }
  m.virtual_tick_to_order_p50_ns = virt.percentile(0.5);
  m.virtual_tick_to_order_p99_ns = virt.percentile(0.99);
  m.fill_ratio = m.orders > 0 ? static_cast<double>(m.fills) / static_cast<double>(m.orders) : 0.0;
  fq.quotes_placed = m.orders;
  fq.fill_rate_per_quote =
      m.orders > 0 ? static_cast<double>(fq.quotes_filled) / static_cast<double>(m.orders) : 0.0;
  if (!ttf_sorted.empty()) {
    std::sort(ttf_sorted.begin(), ttf_sorted.end());
    const auto at = [&](double p) {
      auto i = static_cast<std::size_t>(p * static_cast<double>(ttf_sorted.size()));
      if (i >= ttf_sorted.size()) i = ttf_sorted.size() - 1;
      return static_cast<std::uint64_t>(ttf_sorted[i]);
    };
    fq.time_to_fill_p50_ns = at(0.5);
    fq.time_to_fill_p90_ns = at(0.9);
    fq.time_to_fill_p99_ns = at(0.99);
  }
  return m;
}

}  // namespace fastmm::bt
