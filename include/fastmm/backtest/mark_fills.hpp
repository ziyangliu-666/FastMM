#pragma once
// Marks times against a recorded session's mid: for each row of a CSV of (time, symbol), the
// BookTicker mid at that venue time and at each horizon after it, read in one pass over the
// journal's parts (fastmm-data mark). A live session's fills and any backtest's over the same
// journal, marked against the same recording, compare without either run's own view of the book.
#include <cstdint>
#include <string>
#include <vector>

namespace fastmm::bt {

struct MarkOptions {
  std::vector<std::int64_t> horizons_ns = {0};
  // Also write the mid of every symbol at the end of each bar to this CSV (bar_ns,symbol,mid; a
  // row only when it changed). Empty: none.
  std::string bars_out;
  std::int64_t bar_ns = 60'000'000'000;
  // Our quantity left out of the recorded book tickers (journal format before v5), as a backtest
  // reads them.
  bool strip_own = true;
};

// Each input is a CSV whose header names the columns `t_ns` and `symbol`; `<input>.marks` gets one
// line per row with the mid at t + each horizon, empty where the recording has none (before its
// first ticker, or after its last). Returns the rows marked. Throws std::runtime_error when a file
// cannot be read or written.
std::uint64_t mark_fills(const std::string& journal,
                         const std::vector<std::string>& inputs,
                         const MarkOptions& opt);

}  // namespace fastmm::bt
