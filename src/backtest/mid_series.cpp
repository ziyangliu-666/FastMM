#include "fastmm/backtest/mid_series.hpp"

#include "fastmm/backtest/own_orders.hpp"
#include "fastmm/core/messages.hpp"

#include <algorithm>
#include <iterator>

namespace fastmm::bt {

MidSeries::MidSeries(JournalReader& reader) {
  reader.for_each([&](const EventHeader* h) {
    if (h->type != EventType::BookTicker || (h->flags & EventHeader::kOutbound) != 0) return;
    const auto& m = msg_cast<BookTickerMsg>(h);
    if (!m.bid_px.is_positive() || !m.ask_px.is_positive()) return;
    add(h->instrument, venue_ts(*h), (m.bid_px.to_double() + m.ask_px.to_double()) / 2);
  });
  reader.reset();
  sort();
}

void MidSeries::add(InstrumentId inst, Timestamp t, double mid) {
  const std::uint32_t i = inst.value;
  if (i >= by_inst_.size()) by_inst_.resize(i + 1);
  by_inst_[i].emplace_back(t.ns, mid);
  ++n_;
}

void MidSeries::sort() {
  for (auto& v : by_inst_) {
    if (!std::is_sorted(
            v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; }))
      std::stable_sort(
          v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  }
}

double MidSeries::at(std::uint32_t inst, std::int64_t t) const noexcept {
  if (inst >= by_inst_.size() || by_inst_[inst].empty()) return 0;
  const auto& v = by_inst_[inst];
  if (t > v.back().first) return 0;
  auto it = std::upper_bound(
      v.begin(), v.end(), t, [](std::int64_t x, const auto& e) { return x < e.first; });
  if (it == v.begin()) return 0;
  return std::prev(it)->second;
}

double MidSeries::last(std::uint32_t inst) const noexcept {
  return inst < by_inst_.size() && !by_inst_[inst].empty() ? by_inst_[inst].back().second : 0;
}

}  // namespace fastmm::bt
