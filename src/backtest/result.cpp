#include "fastmm/backtest/result.hpp"

#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"

#include <fmt/format.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fastmm::bt {

namespace {
std::string dec(std::int64_t raw) {
  char buf[kMaxDecimalChars];
  return std::string(buf, Notional::from_raw(raw).to_decimal(buf));
}
const char* side_str(std::int8_t s) noexcept {
  return s == 0 ? "B" : (s == 1 ? "S" : "-");
}
std::string json_escape(std::string_view v) {
  std::string out;
  out.reserve(v.size());
  for (char c : v) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      fmt::format_to(std::back_inserter(out), "\\u{:04x}", static_cast<unsigned>(c));
    } else {
      out += c;
    }
  }
  return out;
}
bool write_file(const std::filesystem::path& p, const std::string& text) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << text;
  return static_cast<bool>(out);
}
}  // namespace

void FillRows::reserve(std::size_t n) {
  ts.reserve(n);
  instrument.reserve(n);
  side.reserve(n);
  price.reserve(n);
  qty.reserve(n);
  fee.reserve(n);
  cl_ord_id.reserve(n);
  liquidity.reserve(n);
  mid.reserve(n);
  best_bid.reserve(n);
  best_ask.reserve(n);
  queue_ahead.reserve(n);
  for (std::vector<std::int64_t>& col : markout_mid) col.reserve(n);
}
void EquityRows::reserve(std::size_t n) {
  ts.reserve(n);
  realized.reserve(n);
  unrealized.reserve(n);
  fees.reserve(n);
  position.reserve(n);
  mid.reserve(n);
  quoted.reserve(n);
}
void OrderRows::reserve(std::size_t n) {
  ts.reserve(n);
  venue_ts.reserve(n);
  trigger_ts.reserve(n);
  cl_ord_id.reserve(n);
  instrument.reserve(n);
  side.reserve(n);
  price.reserve(n);
  qty.reserve(n);
  kind.reserve(n);
  type.reserve(n);
}

std::string BacktestResult::summary_table() const {
  const Metrics& m = metrics;
  std::string s;
  auto row = [&](std::string_view k, const std::string& v) {
    fmt::format_to(std::back_inserter(s), "  {:<30} {}\n", k, v);
  };
  fmt::format_to(std::back_inserter(s),
                 "backtest {}  seed={}  md_events={}  steps={}  wall={:.2f}s\n",
                 strategy,
                 seed,
                 md_events,
                 engine_steps,
                 wall_seconds);
  row("net pnl", fmt::format("{:.4f}", m.net_pnl));
  row("realized / unrealized / fees",
      fmt::format("{:.4f} / {:.4f} / {:.4f}", m.realized_pnl, m.unrealized_pnl, m.fees));
  row("sharpe (per bar / annualized)",
      std::isfinite(m.sharpe_annualized)
          ? fmt::format("{:.4f} / {:.2f}", m.sharpe_bar, m.sharpe_annualized)
          : fmt::format("{:.4f} / n/a (run under 1 day)", m.sharpe_bar));
  row("max drawdown",
      std::isfinite(m.max_drawdown_pct)
          ? fmt::format(
                "{:.4f} ({:.3f}% of initial capital)", m.max_drawdown, m.max_drawdown_pct * 100.0)
          : fmt::format("{:.4f}", m.max_drawdown));
  row("fills (maker / taker)", fmt::format("{} ({} / {})", m.fills, m.maker_fills, m.taker_fills));
  row("orders / cancels / replaces", fmt::format("{} / {} / {}", m.orders, m.cancels, m.replaces));
  row("rejects", fmt::format("{}", m.rejects));
  row("fill ratio", fmt::format("{:.4f}", m.fill_ratio));
  row("spread captured (bps)", fmt::format("{:.3f}", m.spread_captured_bps));
  row("realized spread (bps of notional)",
      fmt::format("{:.3f}", m.fill_quality.realized_spread_bps));
  row("fills at / inside / behind / through",
      fmt::format("{:.1f}% / {:.1f}% / {:.1f}% / {:.1f}%",
                  m.fill_quality.at_touch_share * 100.0,
                  m.fill_quality.inside_touch_share * 100.0,
                  m.fill_quality.behind_touch_share * 100.0,
                  m.fill_quality.through_touch_share * 100.0));
  row("quotes filled / placed",
      fmt::format("{} / {} ({:.1f}%)",
                  m.fill_quality.quotes_filled,
                  m.fill_quality.quotes_placed,
                  m.fill_quality.fill_rate_per_quote * 100.0));
  row("time to fill p50/p90/p99",
      fmt::format("{:.1f} / {:.1f} / {:.1f} ms",
                  static_cast<double>(m.fill_quality.time_to_fill_p50_ns) / 1e6,
                  static_cast<double>(m.fill_quality.time_to_fill_p90_ns) / 1e6,
                  static_cast<double>(m.fill_quality.time_to_fill_p99_ns) / 1e6));
  row("queue ahead at fill p50/p90",
      m.fill_quality.queue_position_known
          ? fmt::format(
                "{:.5f} / {:.5f}", m.fill_quality.queue_ahead_p50, m.fill_quality.queue_ahead_p90)
          : std::string("n/a (the matching fill model has no queue position)"));
  row("volume base / quote", fmt::format("{:.5f} / {:.2f}", m.volume_base, m.volume_quote));
  row("inventory mean / |mean| / max",
      fmt::format(
          "{:.5f} / {:.5f} / {:.5f}", m.inventory_mean, m.inventory_abs_mean, m.inventory_max));
  row("final position", fmt::format("{:.5f}", m.final_position));
  row("quote uptime", fmt::format("{:.1f}%", m.quote_uptime * 100.0));
  row("bars / duration", fmt::format("{} / {:.1f}s", m.bars, m.duration_s));
  row("tick-to-order virtual p50/p99",
      fmt::format("{} / {} ns", m.virtual_tick_to_order_p50_ns, m.virtual_tick_to_order_p99_ns));
  row("tick-to-order wall p50/p99",
      fmt::format("{} / {} ns", m.wall_tick_to_order_p50_ns, m.wall_tick_to_order_p99_ns));
  row("outbound messages / sha256", fmt::format("{} / {}", outbound_messages, outbound_sha256));

  const PnlDecomposition& d = m.decomposition;
  fmt::format_to(std::back_inserter(s),
                 "\nwhere the PnL came from (quote currency, {:.2f} traded notional)\n",
                 d.notional);
  auto part = [&](std::string_view k, double v, const std::string& note) {
    fmt::format_to(std::back_inserter(s), "  {:<30} {:>12.4f}  {}\n", k, v, note);
  };
  part("gross spread capture",
       d.spread_capture,
       d.capture_fills == d.fills
           ? fmt::format(
                 "{:+.3f} bps of {:.2f}, mid at the fill", d.spread_capture_bps, d.capture_notional)
           : fmt::format("{:+.3f} bps of {:.2f}, over the {} of {} fills that had a venue mid",
                         d.spread_capture_bps,
                         d.capture_notional,
                         d.capture_fills,
                         d.fills));
  part("mid drift after the fills",
       d.mid_drift,
       "adverse selection + the open inventory, marked at the final mid");
  part("fees paid", -d.fees_paid, "");
  part("rebates received",
       d.rebates_received,
       d.rebates_received > 0.0 ? "<- a rebate, not edge" : "");
  part("= net", d.net, "");
  part("reported net pnl", m.net_pnl, "");
  part("unexplained", d.residual, "fixed-point rounding, or something this split does not model");

  if (!m.markouts.empty()) {
    s += "\nmarkout per fill (mid at fill + horizon vs the fill price; bps of notional)\n";
    fmt::format_to(std::back_inserter(s),
                   "  {:<8} {:>10} {:>10} {:>10} {:>10} {:>10} {:>8} {:>8} {:>8}\n",
                   "horizon",
                   "markout",
                   "mo bps",
                   "capture",
                   "cap bps",
                   "adv.sel",
                   "fills",
                   "past end",
                   "no mid");
    for (const MarkoutHorizon& h : m.markouts) {
      fmt::format_to(std::back_inserter(s),
                     "  {:<8} {:>10.4f} {:>10.4f} {:>10.4f} {:>10.4f} {:>10.4f} {:>8} {:>8} "
                     "{:>8}\n",
                     h.label(),
                     h.total.markout_quote(),
                     h.total.markout_bps(),
                     h.total.capture_quote(),
                     h.total.capture_bps(),
                     h.total.adverse_selection_bps(),
                     h.total.fills,
                     h.excluded_past_end,
                     h.excluded_no_mid);
      if (h.total.fills == 0) continue;  // nothing measured at this horizon
      fmt::format_to(std::back_inserter(s),
                     "  {:<8} {:>10.4f} {:>10.4f} {:>10.4f} {:>10.4f}\n",
                     "  buy",
                     h.buy.markout_quote(),
                     h.buy.markout_bps(),
                     h.buy.capture_quote(),
                     h.buy.capture_bps());
      fmt::format_to(std::back_inserter(s),
                     "  {:<8} {:>10.4f} {:>10.4f} {:>10.4f} {:>10.4f}\n",
                     "  sell",
                     h.sell.markout_quote(),
                     h.sell.markout_bps(),
                     h.sell.capture_quote(),
                     h.sell.capture_bps());
      for (std::size_t k = 0; k < h.instrument.size(); ++k) {
        if (h.instrument[k].fills == 0) continue;
        if (h.instrument.size() <= 1) break;  // one instrument: the total already says it
        fmt::format_to(std::back_inserter(s),
                       "  {:<8} {:>10.4f} {:>10.4f} {:>10.4f} {:>10.4f} {:>10.4f} {:>8}\n",
                       fmt::format("  #{}", k),
                       h.instrument[k].markout_quote(),
                       h.instrument[k].markout_bps(),
                       h.instrument[k].capture_quote(),
                       h.instrument[k].capture_bps(),
                       h.instrument[k].adverse_selection_bps(),
                       h.instrument[k].fills);
      }
    }
    s += "  a positive markout means the mid kept moving your way after the fill; a capture that\n"
         "  is positive while the markout is negative is adverse selection, not edge. Excluded\n"
         "  fills are never marked at a substitute price: 'past end' is a horizon beyond the last\n"
         "  event, 'no mid' a venue book with only one side there.\n";
  }
  return s;
}

std::string BacktestResult::summary_json() const {
  const Metrics& m = metrics;
  std::string s = "{\n";
  auto num = [&](std::string_view k, double v) {
    fmt::format_to(std::back_inserter(s),
                   "  \"{}\": {},\n",
                   k,
                   std::isfinite(v) ? fmt::format("{:.10g}", v) : std::string("null"));
  };
  auto u64 = [&](std::string_view k, std::uint64_t v) {
    fmt::format_to(std::back_inserter(s), "  \"{}\": {},\n", k, v);
  };
  auto i64 = [&](std::string_view k, std::int64_t v) {
    fmt::format_to(std::back_inserter(s), "  \"{}\": {},\n", k, v);
  };
  auto str = [&](std::string_view k, std::string_view v) {
    fmt::format_to(std::back_inserter(s), "  \"{}\": \"{}\",\n", k, json_escape(v));
  };
  str("strategy", strategy);
  s += "  \"params\": {";
  bool first = true;
  for (const auto& [k, v] : params) {
    fmt::format_to(std::back_inserter(s),
                   "{}\"{}\": \"{}\"",
                   first ? "" : ", ",
                   json_escape(k),
                   json_escape(v));
    first = false;
  }
  s += "},\n";
  u64("seed", seed);
  num("net_pnl", m.net_pnl);
  num("realized_pnl", m.realized_pnl);
  num("unrealized_pnl", m.unrealized_pnl);
  num("fees", m.fees);
  num("final_position", m.final_position);
  num("sharpe_bar", m.sharpe_bar);
  num("sharpe_annualized", m.sharpe_annualized);
  num("max_drawdown", m.max_drawdown);
  num("max_drawdown_pct", m.max_drawdown_pct);
  u64("fills", m.fills);
  u64("maker_fills", m.maker_fills);
  u64("taker_fills", m.taker_fills);
  u64("orders", m.orders);
  u64("cancels", m.cancels);
  u64("replaces", m.replaces);
  u64("rejects", m.rejects);
  num("fill_ratio", m.fill_ratio);
  num("spread_captured_bps", m.spread_captured_bps);
  num("volume_base", m.volume_base);
  num("volume_quote", m.volume_quote);
  num("inventory_mean", m.inventory_mean);
  num("inventory_abs_mean", m.inventory_abs_mean);
  num("inventory_max", m.inventory_max);
  num("quote_uptime", m.quote_uptime);
  u64("bars", m.bars);
  num("duration_s", m.duration_s);
  u64("virtual_tick_to_order_p50_ns", m.virtual_tick_to_order_p50_ns);
  u64("virtual_tick_to_order_p99_ns", m.virtual_tick_to_order_p99_ns);
  u64("wall_tick_to_order_p50_ns", m.wall_tick_to_order_p50_ns);
  u64("wall_tick_to_order_p99_ns", m.wall_tick_to_order_p99_ns);
  num("realized_spread_bps", m.fill_quality.realized_spread_bps);
  num("realized_spread_quote", m.fill_quality.realized_spread_quote);
  num("at_touch_share", m.fill_quality.at_touch_share);
  num("inside_touch_share", m.fill_quality.inside_touch_share);
  num("behind_touch_share", m.fill_quality.behind_touch_share);
  num("through_touch_share", m.fill_quality.through_touch_share);
  u64("time_to_fill_p50_ns", m.fill_quality.time_to_fill_p50_ns);
  u64("time_to_fill_p90_ns", m.fill_quality.time_to_fill_p90_ns);
  u64("time_to_fill_p99_ns", m.fill_quality.time_to_fill_p99_ns);
  num("fill_rate_per_quote", m.fill_quality.fill_rate_per_quote);
  u64("quotes_placed", m.fill_quality.quotes_placed);
  u64("quotes_filled", m.fill_quality.quotes_filled);
  if (m.fill_quality.queue_position_known) {
    num("queue_ahead_mean", m.fill_quality.queue_ahead_mean);
    num("queue_ahead_p50", m.fill_quality.queue_ahead_p50);
    num("queue_ahead_p90", m.fill_quality.queue_ahead_p90);
  }
  s += "  \"pnl_decomposition\": {";
  fmt::format_to(std::back_inserter(s),
                 "\"spread_capture\": {:.10g}, \"spread_capture_bps\": {:.10g}, "
                 "\"mid_drift\": {:.10g}, \"fees_paid\": {:.10g}, "
                 "\"rebates_received\": {:.10g}, \"net\": {:.10g}, \"residual\": {:.10g}, "
                 "\"notional\": {:.10g}, \"capture_notional\": {:.10g}, "
                 "\"capture_fills\": {}, \"fills\": {}",
                 m.decomposition.spread_capture,
                 m.decomposition.spread_capture_bps,
                 m.decomposition.mid_drift,
                 m.decomposition.fees_paid,
                 m.decomposition.rebates_received,
                 m.decomposition.net,
                 m.decomposition.residual,
                 m.decomposition.notional,
                 m.decomposition.capture_notional,
                 m.decomposition.capture_fills,
                 m.decomposition.fills);
  s += "},\n";
  s += "  \"markouts\": [";
  for (std::size_t j = 0; j < m.markouts.size(); ++j) {
    const MarkoutHorizon& h = m.markouts[j];
    auto fields = [](const MarkoutBucket& b) {
      return fmt::format(
          "\"markout\": {:.10g}, \"markout_bps\": {:.10g}, \"capture\": {:.10g}, "
          "\"capture_bps\": {:.10g}, \"adverse_selection\": {:.10g}, "
          "\"adverse_selection_bps\": {:.10g}, \"notional\": {:.10g}, \"fills\": {}",
          b.markout_quote(),
          b.markout_bps(),
          b.capture_quote(),
          b.capture_bps(),
          b.adverse_selection_quote(),
          b.adverse_selection_bps(),
          b.notional(),
          b.fills);
    };
    auto bucket = [&](std::string_view name, const MarkoutBucket& b) {
      return fmt::format("\"{}\": {{{}}}", name, fields(b));
    };
    fmt::format_to(std::back_inserter(s),
                   "{}\n    {{\"horizon_ns\": {}, \"label\": \"{}\", \"excluded_fills\": {}, "
                   "\"excluded_past_end\": {}, \"excluded_no_mid\": {}, {}, {}, {}, {}, {}",
                   j == 0 ? "" : ",",
                   h.horizon_ns,
                   h.label(),
                   h.excluded_fills,
                   h.excluded_past_end,
                   h.excluded_no_mid,
                   bucket("total", h.total),
                   bucket("buy", h.buy),
                   bucket("sell", h.sell),
                   bucket("maker", h.maker),
                   bucket("taker", h.taker));
    s += ", \"instrument\": [";
    for (std::size_t k = 0; k < h.instrument.size(); ++k) {
      fmt::format_to(std::back_inserter(s),
                     "{}{{\"instrument\": {}, {}}}",
                     k == 0 ? "" : ", ",
                     k,
                     fields(h.instrument[k]));
    }
    s += "]}";
  }
  s += m.markouts.empty() ? "],\n" : "\n  ],\n";
  u64("md_events", md_events);
  u64("engine_steps", engine_steps);
  u64("outbound_messages", outbound_messages);
  str("outbound_sha256", outbound_sha256);
  i64("start_ts", start_ts);
  i64("end_ts", end_ts);
  fmt::format_to(std::back_inserter(s), "  \"wall_seconds\": {:.6f}\n}}\n", wall_seconds);
  return s;
}

std::string BacktestResult::equity_csv() const {
  std::string s = "ts_ns,equity,realized,unrealized,fees,position,mid,quoted\n";
  for (std::size_t i = 0; i < equity.size(); ++i) {
    fmt::format_to(std::back_inserter(s),
                   "{},{},{},{},{},{},{},{}\n",
                   equity.ts[i],
                   dec(equity.equity(i)),
                   dec(equity.realized[i]),
                   dec(equity.unrealized[i]),
                   dec(equity.fees[i]),
                   dec(equity.position[i]),
                   dec(equity.mid[i]),
                   static_cast<unsigned>(equity.quoted[i]));
  }
  return s;
}

std::string BacktestResult::fills_csv() const {
  std::string s =
      "ts_ns,inst,side,price,qty,fee,cl_ord_id,liquidity,mid,best_bid,best_ask,"
      "queue_ahead";
  for (const MarkoutHorizon& h : metrics.markouts)
    fmt::format_to(std::back_inserter(s), ",mid_{}", h.label());
  s += '\n';
  for (std::size_t i = 0; i < fills.size(); ++i) {
    fmt::format_to(std::back_inserter(s),
                   "{},{},{},{},{},{},{},{},{},{},{},{}",
                   fills.ts[i],
                   fills.instrument[i],
                   side_str(fills.side[i]),
                   dec(fills.price[i]),
                   dec(fills.qty[i]),
                   dec(fills.fee[i]),
                   fills.cl_ord_id[i],
                   fills.liquidity[i] == static_cast<std::uint8_t>(Liquidity::Taker) ? "T" : "M",
                   dec(fills.mid[i]),
                   dec(fills.best_bid[i]),
                   dec(fills.best_ask[i]),
                   fills.queue_ahead[i] < 0 ? std::string("") : dec(fills.queue_ahead[i]));
    // Empty when the run ended before the horizon: that fill is excluded, not marked at the
    // last known mid.
    for (std::size_t j = 0; j < metrics.markouts.size() && j < fills.markout_mid.size(); ++j) {
      const std::int64_t mid_h = fills.markout_mid[j][i];
      fmt::format_to(std::back_inserter(s), ",{}", mid_h > 0 ? dec(mid_h) : std::string());
    }
    s += '\n';
  }
  return s;
}

std::string BacktestResult::orders_csv() const {
  static constexpr const char* kKind[] = {"new", "cancel", "replace"};
  std::string s = "ts_ns,venue_ts_ns,trigger_ts_ns,kind,cl_ord_id,inst,side,price,qty,type\n";
  for (std::size_t i = 0; i < orders.size(); ++i) {
    const bool is_new = orders.kind[i] == kOrderKindNew;
    fmt::format_to(
        std::back_inserter(s),
        "{},{},{},{},{},{},{},{},{},{}\n",
        orders.ts[i],
        orders.venue_ts[i],
        orders.trigger_ts[i],
        kKind[orders.kind[i] < 3 ? orders.kind[i] : 0],
        orders.cl_ord_id[i],
        orders.instrument[i],
        side_str(orders.side[i]),
        dec(orders.price[i]),
        dec(orders.qty[i]),
        is_new ? to_string(static_cast<OrderType>(orders.type[i])) : std::string_view("-"));
  }
  return s;
}

bool BacktestResult::write_all(const std::string& dir) const {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) return false;
  const std::filesystem::path d(dir);
  return write_file(d / "equity.csv", equity_csv()) && write_file(d / "fills.csv", fills_csv()) &&
         write_file(d / "orders.csv", orders_csv()) &&
         write_file(d / "summary.json", summary_json());
}

}  // namespace fastmm::bt
