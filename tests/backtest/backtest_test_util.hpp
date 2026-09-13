#pragma once
// Shared helpers for the backtest tests: configs, event -> CSV / array conversion.
#include "test_support.hpp"

#include "fastmm/backtest/array_source.hpp"
#include "fastmm/backtest/backtest_config.hpp"
#include "fastmm/backtest/data_source.hpp"
#include "fastmm/backtest/result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fastmm::bt::testutil {

inline Price px(const char* s) {
  return Price::from_decimal(s).value();
}
inline Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

// BTCUSDT-like instrument, BasicMM parameters quoting ~2 ticks inside a busy synthetic market.
inline BacktestConfig synthetic_config(std::uint64_t seed, Duration duration) {
  BacktestConfig c = BacktestConfig::single_instrument("BTCUSDT", px("0.01"), qt("0.00001"));
  c.set_seed(seed);
  c.duration = duration;
  c.engine.risk.max_order_qty = qt("0.01");
  c.engine.risk.max_position = qt("0.05");
  c.engine.risk.max_open_orders = 8;
  c.transport.fees = sim::FeeModel::from_bps(-0.5, 3.0);
  c.transport.stp = sim::StpMode::CancelTaker;
  c.generator.limit_rate_per_s = 300;
  c.generator.market_rate_per_s = 40;
  c.generator.mid_step_rate_per_s = 5;
  c.generator.market_qty_median_lots = 300;
  // skew 0: BasicMM skews by bps of mid per quote_qty, which at 60000 is hundreds of ticks
  // and would push the unwinding quote through the book (post-only rejects).
  c.params = {{"half_spread_bps", "0.003"},
              {"skew_bps_per_unit", "0"},
              {"quote_qty", "0.002"},
              {"max_inventory", "0.02"},
              {"requote_threshold_ticks", "1"},
              {"pull_on_stale_ms", "0"}};
  c.measure_wall_clock = false;
  return c;
}

inline std::string decimal(std::int64_t raw) {
  char buf[kMaxDecimalChars];
  return std::string(buf, Price::from_raw(raw).to_decimal(buf));
}

// One normalized event as CsvSource rows (`ts_ns,type,inst,side,price,qty,seq`).
inline std::string to_csv_rows(const EventHeader& h) {
  const std::int64_t ts = h.exch_ts.valid() ? h.exch_ts.ns : h.recv_ts.ns;
  const std::string pre = std::to_string(ts) + ",";
  const std::string inst = "," + std::to_string(h.instrument.value) + ",";
  std::string out;
  auto row = [&](const char* type, const char* side, Price p, Qty q, std::uint64_t seq) {
    out += pre + type + inst + side + "," + decimal(p.raw) + "," + decimal(q.raw) + "," +
           std::to_string(seq) + "\n";
  };
  switch (h.type) {
    case EventType::BookDelta:
    case EventType::BookSnapshot: {
      const auto& d = msg_cast<BookDeltaMsg>(&h);
      const char* type = d.is_snapshot() ? "S" : "D";
      for (const Level& l : d.bids()) row(type, "B", l.price, l.qty, d.last_update_id);
      for (const Level& l : d.asks()) row(type, "A", l.price, l.qty, d.last_update_id);
      break;
    }
    case EventType::Trade: {
      const auto& t = msg_cast<TradeMsg>(&h);
      row("T", t.aggressor == Side::Buy ? "B" : "A", t.price, t.qty, t.trade_id);
      break;
    }
    case EventType::BookTicker: {
      const auto& t = msg_cast<BookTickerMsg>(&h);
      row("B", "B", t.bid_px, t.bid_qty, h.venue_seq);
      row("B", "A", t.ask_px, t.ask_qty, h.venue_seq);
      break;
    }
    default:
      break;
  }
  return out;
}

inline std::string source_to_csv(MdSource& src) {
  std::string text = "ts_ns,type,inst,side,price,qty,seq\n";
  src.reset();
  while (const EventHeader* h = src.next()) text += to_csv_rows(*h);
  src.reset();
  return text;
}

// Owning column storage for ArraySource built from CSV text.
struct OwnedColumns {
  std::vector<std::int64_t> ts;
  std::vector<std::uint8_t> type;
  std::vector<std::uint32_t> inst;
  std::vector<std::int8_t> side;
  std::vector<std::int64_t> price;
  std::vector<std::int64_t> qty;
  std::vector<double> price_f;
  std::vector<double> qty_f;
  std::vector<std::uint64_t> seq;

  static OwnedColumns from_csv(const std::string& text);
  [[nodiscard]] ArrayColumns view(bool doubles) const {
    ArrayColumns c;
    c.ts = ts;
    c.type = type;
    c.inst = inst;
    c.side = side;
    if (doubles) {
      c.price_f64 = price_f;
      c.qty_f64 = qty_f;
    } else {
      c.price_i64 = price;
      c.qty_i64 = qty;
    }
    c.seq = seq;
    return c;
  }
};

}  // namespace fastmm::bt::testutil

#include "fastmm/backtest/csv_source.hpp"

namespace fastmm::bt::testutil {
inline OwnedColumns OwnedColumns::from_csv(const std::string& text) {
  OwnedColumns c;
  std::size_t pos = 0;
  Row r;
  while (pos < text.size()) {
    std::size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    if (CsvSource::parse_row(std::string_view(text).substr(pos, nl - pos), r)) {
      c.ts.push_back(r.ts);
      c.type.push_back(static_cast<std::uint8_t>(r.type));
      c.inst.push_back(r.inst);
      c.side.push_back(static_cast<std::int8_t>(r.side));
      c.price.push_back(r.price.raw);
      c.qty.push_back(r.qty.raw);
      c.price_f.push_back(r.price.to_double());
      c.qty_f.push_back(r.qty.to_double());
      c.seq.push_back(r.seq);
    }
    pos = nl + 1;
  }
  return c;
}
}  // namespace fastmm::bt::testutil
