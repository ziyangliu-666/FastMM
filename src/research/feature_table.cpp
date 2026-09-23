#include "fastmm/research/feature_table.hpp"

#include "fastmm/core/fixed_point.hpp"

#include <fmt/format.h>

#include <iterator>

namespace fastmm::research {

namespace {
std::string dec(std::int64_t raw) {
  char buf[kMaxDecimalChars];
  return std::string(buf, Notional::from_raw(raw).to_decimal(buf));
}
}  // namespace

void FeatureRows::reserve(std::size_t n) {
  ts.reserve(n);
  instrument.reserve(n);
  mid.reserve(n);
  microprice.reserve(n);
  best_bid.reserve(n);
  best_ask.reserve(n);
  bid_qty.reserve(n);
  ask_qty.reserve(n);
  imbalance.reserve(n);
  spread.reserve(n);
  for (std::vector<std::int64_t>& col : forward_mid) col.reserve(n);
}

void FeatureRows::clear() noexcept {
  ts.clear();
  instrument.clear();
  mid.clear();
  microprice.clear();
  best_bid.clear();
  best_ask.clear();
  bid_qty.clear();
  ask_qty.clear();
  imbalance.clear();
  spread.clear();
  for (std::vector<std::int64_t>& col : forward_mid) col.clear();
}

std::string horizon_label(std::int64_t ns) {
  if (ns % 60'000'000'000LL == 0) return fmt::format("{}m", ns / 60'000'000'000LL);
  if (ns % 1'000'000'000LL == 0) return fmt::format("{}s", ns / 1'000'000'000LL);
  if (ns % 1'000'000LL == 0) return fmt::format("{}ms", ns / 1'000'000LL);
  return fmt::format("{}us", ns / 1'000LL);
}

std::string FeatureTable::summary_table() const {
  std::string s;
  fmt::format_to(std::back_inserter(s), "rows                {}\n", rows.size());
  fmt::format_to(std::back_inserter(s), "events              {}\n", events);
  fmt::format_to(std::back_inserter(s), "book updates        {}\n", book_updates);
  fmt::format_to(std::back_inserter(s), "skipped one-sided   {}\n", skipped_one_sided);
  fmt::format_to(std::back_inserter(s), "skipped subsample   {}\n", skipped_subsample);
  fmt::format_to(std::back_inserter(s), "start ts            {}\n", start_ts);
  fmt::format_to(std::back_inserter(s), "end ts              {}\n", end_ts);
  if (coverage.empty()) return s;
  s += "\nhorizon   resolved   past end    no mid\n";
  for (const HorizonCoverage& c : coverage) {
    fmt::format_to(std::back_inserter(s),
                   "{:>7}  {:>9}  {:>9}  {:>8}\n",
                   c.label(),
                   c.resolved,
                   c.excluded_past_end,
                   c.excluded_no_mid);
  }
  return s;
}

std::string FeatureTable::csv() const {
  std::string s = "ts_ns,inst,mid,microprice,best_bid,best_ask,bid_qty,ask_qty,imbalance,spread";
  for (const HorizonCoverage& c : coverage)
    fmt::format_to(std::back_inserter(s), ",mid_{}", c.label());
  s += '\n';
  for (std::size_t i = 0; i < rows.size(); ++i) {
    fmt::format_to(std::back_inserter(s),
                   "{},{},{},{},{},{},{},{},{},{}",
                   rows.ts[i],
                   rows.instrument[i],
                   dec(rows.mid[i]),
                   dec(rows.microprice[i]),
                   dec(rows.best_bid[i]),
                   dec(rows.best_ask[i]),
                   dec(rows.bid_qty[i]),
                   dec(rows.ask_qty[i]),
                   dec(rows.imbalance[i]),
                   dec(rows.spread[i]));
    // Empty where the row has no forward mid: it is excluded from that horizon, not marked at
    // the last known mid.
    for (const std::vector<std::int64_t>& col : rows.forward_mid) {
      const std::int64_t v = col[i];
      if (v == 0) {
        s += ',';
      } else {
        fmt::format_to(std::back_inserter(s), ",{}", dec(v));
      }
    }
    s += '\n';
  }
  return s;
}

}  // namespace fastmm::research
