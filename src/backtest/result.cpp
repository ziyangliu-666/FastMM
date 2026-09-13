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
      fmt::format("{:.4f} / {:.2f}", m.sharpe_bar, m.sharpe_annualized));
  row("max drawdown", fmt::format("{:.4f} ({:.3f}%)", m.max_drawdown, m.max_drawdown_pct * 100.0));
  row("fills (maker / taker)", fmt::format("{} ({} / {})", m.fills, m.maker_fills, m.taker_fills));
  row("orders / cancels / replaces", fmt::format("{} / {} / {}", m.orders, m.cancels, m.replaces));
  row("rejects", fmt::format("{}", m.rejects));
  row("fill ratio", fmt::format("{:.4f}", m.fill_ratio));
  row("spread captured (bps)", fmt::format("{:.3f}", m.spread_captured_bps));
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
  std::string s = "ts_ns,inst,side,price,qty,fee,cl_ord_id,liquidity,mid\n";
  for (std::size_t i = 0; i < fills.size(); ++i) {
    fmt::format_to(std::back_inserter(s),
                   "{},{},{},{},{},{},{},{},{}\n",
                   fills.ts[i],
                   fills.instrument[i],
                   side_str(fills.side[i]),
                   dec(fills.price[i]),
                   dec(fills.qty[i]),
                   dec(fills.fee[i]),
                   fills.cl_ord_id[i],
                   fills.liquidity[i] == static_cast<std::uint8_t>(Liquidity::Taker) ? "T" : "M",
                   dec(fills.mid[i]));
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
