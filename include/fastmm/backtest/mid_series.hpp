#pragma once
// The venue mid of each instrument over a recorded session, for marking fills against: the
// BookTicker stream's (bid + ask) / 2 in venue time, as recorded (our own quotes included, as a
// live session's PnL report marks them). The fill check (fill_check.hpp) and the backtest
// comparison (calibrate.hpp) mark live fills, model fills and backtest fills against the same one.
#include "fastmm/core/journal.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace fastmm::bt {

class MidSeries {
 public:
  MidSeries() = default;
  // Every BookTicker of the journal with both sides; resets the reader.
  explicit MidSeries(JournalReader& reader);

  // Adds a mid; sort() once after the last.
  void add(InstrumentId inst, Timestamp t, double mid);
  void sort();

  // The mid at venue time t: the last one at or before t; 0 before the first and after the last
  // (a horizon past the data is not marked).
  [[nodiscard]] double at(std::uint32_t inst, std::int64_t t) const noexcept;
  [[nodiscard]] double last(std::uint32_t inst) const noexcept;
  [[nodiscard]] bool empty() const noexcept { return n_ == 0; }

 private:
  std::vector<std::vector<std::pair<std::int64_t, double>>> by_inst_;
  std::uint64_t n_ = 0;
};

}  // namespace fastmm::bt
