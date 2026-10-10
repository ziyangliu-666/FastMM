#include "fastmm/backtest/live_analysis.hpp"

#include "fastmm/backtest/mid_series.hpp"
#include "fastmm/backtest/own_orders.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/core/messages.hpp"

#include <fmt/format.h>

#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>
#include <tuple>

namespace fastmm::bt {

namespace {

std::int64_t to_raw(double quote) {
  return std::llround(quote * 1e8);
}

// A number for JSON: NaN and infinities have no spelling there.
std::string num(double v) {
  return std::isfinite(v) ? fmt::format("{:.6g}", v) : std::string("null");
}

std::string json_text(std::string_view v) {
  std::string out = "\"";
  for (const char c : v) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

std::string bucket_json(const MarkoutBucket& b) {
  return fmt::format(
      "{{\"fills\": {}, \"notional\": {}, \"capture_bps\": {}, \"markout_bps\": {}, "
      "\"adverse_selection_bps\": {}, \"markout\": {}}}",
      b.fills,
      num(b.notional()),
      num(b.capture_bps()),
      num(b.markout_bps()),
      num(b.adverse_selection_bps()),
      num(b.markout_quote()));
}

}  // namespace

LiveAnalysis analyze_live(const std::string& path, std::span<const std::int64_t> horizons_ns) {
  JournalReader reader;
  if (auto r = reader.open(path); !r)
    throw std::runtime_error("cannot open " + path + ": " + reader.describe(r.error()));
  LiveAnalysis a;
  const auto& strategy = reader.header().strategy;
  a.strategy.assign(strategy, strnlen(strategy, sizeof strategy));
  const std::span<const Instrument> table = reader.instruments();
  a.instruments.resize(table.size());
  for (std::size_t i = 0; i < table.size(); ++i)
    a.instruments[i].symbol = std::string(table[i].symbol.view());
  const MidSeries mids(reader);
  a.mids = !mids.empty();
  for (const std::int64_t h : horizons_ns) {
    MarkoutHorizon m;
    m.horizon_ns = h;
    m.instrument.resize(table.size());
    a.horizons.push_back(std::move(m));
  }

  std::set<std::tuple<std::uint32_t, Side, std::string>> seen;
  reader.for_each([&](const EventHeader* h) {
    const std::int64_t t = venue_ts(*h).ns;
    if (t != 0) {
      if (a.first_ns == 0 || t < a.first_ns) a.first_ns = t;
      if (t > a.last_ns) a.last_ns = t;
    }
    switch (h->type) {
      case EventType::OutNewOrder:
        ++a.orders;
        return;
      case EventType::OutCancel:
        ++a.cancels;
        return;
      case EventType::OutReplace:
        ++a.replaces;
        return;
      case EventType::OrderReject:
        ++a.rejects;
        return;
      case EventType::OrderFill:
        break;
      default:
        return;
    }
    const auto& f = msg_cast<OrderFillMsg>(h);
    const std::uint32_t inst = h->instrument.value;
    if (!f.exec_id.empty() && !seen.emplace(inst, f.side, std::string(f.exec_id.view())).second) {
      ++a.repeated_fills;
      return;
    }
    ++a.fills;
    const bool maker = f.liquidity == Liquidity::Maker;
    a.maker_fills += maker ? 1U : 0U;
    const double px = f.price.to_double();
    const double qty = f.qty.to_double();
    if (inst < a.instruments.size()) {
      LiveAnalysisInstrument& x = a.instruments[inst];
      ++x.fills;
      x.maker_fills += maker ? 1U : 0U;
      (f.side == Side::Buy ? x.bought : x.sold) += qty;
      x.notional += px * qty;
    }
    const double s = f.side == Side::Buy ? qty : -qty;
    const double m0 = mids.at(inst, t);
    for (MarkoutHorizon& m : a.horizons) {
      const double mh = mids.at(inst, t + m.horizon_ns);
      if (m0 <= 0) {
        ++m.excluded_no_mid;
        ++m.excluded_fills;
        continue;
      }
      if (mh <= 0) {
        ++m.excluded_past_end;
        ++m.excluded_fills;
        continue;
      }
      const std::int64_t markout = to_raw(s * (mh - px));
      const std::int64_t capture = to_raw(s * (m0 - px));
      const std::int64_t notional = to_raw(px * qty);
      m.total.add(markout, capture, notional);
      (f.side == Side::Buy ? m.buy : m.sell).add(markout, capture, notional);
      (maker ? m.maker : m.taker).add(markout, capture, notional);
      if (inst < m.instrument.size()) m.instrument[inst].add(markout, capture, notional);
    }
  });
  return a;
}

std::string format_live_analysis(const LiveAnalysis& a) {
  std::string out;
  auto it = std::back_inserter(out);
  fmt::format_to(it,
                 "session  strategy '{}', {:.0f} s of venue time\n"
                 "orders   {} sent, {} cancels, {} replaces, {} refused by the venue\n"
                 "fills    {} ({} maker, {:.1f}%), {} repeated report(s) counted once\n",
                 a.strategy,
                 static_cast<double>(a.last_ns - a.first_ns) / 1e9,
                 a.orders,
                 a.cancels,
                 a.replaces,
                 a.rejects,
                 a.fills,
                 a.maker_fills,
                 a.fills == 0
                     ? 0.0
                     : 100.0 * static_cast<double>(a.maker_fills) / static_cast<double>(a.fills),
                 a.repeated_fills);
  fmt::format_to(it,
                 "\n{:<14} {:>7} {:>7} {:>14} {:>14} {:>16}\n",
                 "instrument",
                 "fills",
                 "maker",
                 "bought",
                 "sold",
                 "notional");
  for (const LiveAnalysisInstrument& x : a.instruments) {
    if (x.fills == 0) continue;
    fmt::format_to(it,
                   "{:<14} {:>7} {:>7} {:>14.8g} {:>14.8g} {:>16.2f}\n",
                   x.symbol,
                   x.fills,
                   x.maker_fills,
                   x.bought,
                   x.sold,
                   x.notional);
  }
  if (!a.mids) {
    out += "\nno BookTicker stream in the journal: no mid to mark the fills against\n";
    return out;
  }
  fmt::format_to(it,
                 "\n{:<8} {:<14} {:>7} {:>12} {:>12} {:>12} {:>9}\n",
                 "horizon",
                 "fills of",
                 "marked",
                 "capture_bps",
                 "markout_bps",
                 "adverse_bps",
                 "excluded");
  for (const MarkoutHorizon& m : a.horizons) {
    const auto row = [&](std::string_view what, const MarkoutBucket& b, std::uint64_t excluded) {
      fmt::format_to(it,
                     "{:<8} {:<14} {:>7} {:>12.2f} {:>12.2f} {:>12.2f} {:>9}\n",
                     m.label(),
                     what,
                     b.fills,
                     b.capture_bps(),
                     b.markout_bps(),
                     b.adverse_selection_bps(),
                     excluded);
    };
    row("all", m.total, m.excluded_fills);
    if (m.maker.fills != 0 && m.taker.fills != 0) {
      row("maker", m.maker, 0);
      row("taker", m.taker, 0);
    }
    for (std::size_t i = 0; i < m.instrument.size(); ++i) {
      if (m.instrument[i].fills != 0 && a.instruments.size() > 1)
        row(a.instruments[i].symbol, m.instrument[i], 0);
    }
  }
  return out;
}

std::string live_analysis_json(const LiveAnalysis& a) {
  std::string out;
  auto it = std::back_inserter(out);
  fmt::format_to(it,
                 "{{\"strategy\": {}, \"first_ns\": {}, \"last_ns\": {}, \"orders\": {}, "
                 "\"cancels\": {}, \"replaces\": {}, \"rejects\": {}, \"fills\": {}, "
                 "\"maker_fills\": {}, \"repeated_fills\": {}, \"mids\": {}, \"instruments\": [",
                 json_text(a.strategy),
                 a.first_ns,
                 a.last_ns,
                 a.orders,
                 a.cancels,
                 a.replaces,
                 a.rejects,
                 a.fills,
                 a.maker_fills,
                 a.repeated_fills,
                 a.mids ? "true" : "false");
  bool first = true;
  for (const LiveAnalysisInstrument& x : a.instruments) {
    if (x.fills == 0) continue;
    fmt::format_to(it,
                   "{}{{\"symbol\": {}, \"fills\": {}, \"maker_fills\": {}, \"bought\": {}, "
                   "\"sold\": {}, \"notional\": {}}}",
                   first ? "" : ", ",
                   json_text(x.symbol),
                   x.fills,
                   x.maker_fills,
                   num(x.bought),
                   num(x.sold),
                   num(x.notional));
    first = false;
  }
  out += "], \"horizons\": [";
  for (std::size_t k = 0; k < a.horizons.size(); ++k) {
    const MarkoutHorizon& m = a.horizons[k];
    fmt::format_to(it,
                   "{}{{\"horizon_ns\": {}, \"excluded_no_mid\": {}, \"excluded_past_end\": {}, "
                   "\"all\": {}, \"maker\": {}, \"taker\": {}, \"buy\": {}, \"sell\": {}, "
                   "\"instruments\": {{",
                   k == 0 ? "" : ", ",
                   m.horizon_ns,
                   m.excluded_no_mid,
                   m.excluded_past_end,
                   bucket_json(m.total),
                   bucket_json(m.maker),
                   bucket_json(m.taker),
                   bucket_json(m.buy),
                   bucket_json(m.sell));
    bool any = false;
    for (std::size_t i = 0; i < m.instrument.size(); ++i) {
      if (m.instrument[i].fills == 0) continue;
      fmt::format_to(it,
                     "{}{}: {}",
                     any ? ", " : "",
                     json_text(a.instruments[i].symbol),
                     bucket_json(m.instrument[i]));
      any = true;
    }
    out += "}}";
  }
  out += "]}\n";
  return out;
}

}  // namespace fastmm::bt
