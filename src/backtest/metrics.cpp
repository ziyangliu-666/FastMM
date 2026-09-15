#include "fastmm/backtest/metrics.hpp"

#include "fastmm/backtest/result.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/latency.hpp"

#include <cmath>
#include <limits>

namespace fastmm::bt {

namespace {
double dec(std::int64_t raw) noexcept {
  return static_cast<double>(raw) / static_cast<double>(kFixedScale);
}
}  // namespace

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

  // ---- fills ------------------------------------------------------------------------------
  m.fills = fills.size();
  double spread_sum = 0.0;
  std::uint64_t spread_n = 0;
  for (std::size_t i = 0; i < fills.size(); ++i) {
    if (fills.liquidity[i] == static_cast<std::uint8_t>(Liquidity::Taker)) {
      ++m.taker_fills;
    } else {
      ++m.maker_fills;
    }
    const double q = dec(fills.qty[i]);
    const double px = dec(fills.price[i]);
    m.volume_base += q;
    m.volume_quote += q * px;
    const double mid = dec(fills.mid[i]);
    if (mid > 0.0) {
      const double edge = fills.side[i] == 0 ? (mid - px) : (px - mid);
      spread_sum += edge / mid * 1e4;
      ++spread_n;
    }
  }
  m.spread_captured_bps = spread_n > 0 ? spread_sum / static_cast<double>(spread_n) : 0.0;

  // ---- orders -----------------------------------------------------------------------------
  LogLinearHistogram virt;
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
  }
  m.virtual_tick_to_order_p50_ns = virt.percentile(0.5);
  m.virtual_tick_to_order_p99_ns = virt.percentile(0.99);
  m.fill_ratio = m.orders > 0 ? static_cast<double>(m.fills) / static_cast<double>(m.orders) : 0.0;
  return m;
}

}  // namespace fastmm::bt
