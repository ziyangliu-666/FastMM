#pragma once
// A session's own order traffic, one CSV row per event (fastmm-data events): what the strategy
// sent (new orders, cancels, replaces), what the venues answered (acks, rejects, cancel acks and
// rejects, fills, expiries), the balances they reported, the reconciled positions and the
// parameter updates, in journal order with the times each record carries. Market data is left
// out. The same rows come out of a live session's journal and a backtest's (`journal_out`), so
// the two can be matched order by order.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace fastmm::bt {

struct OrderEventsOptions {
  // Only events whose receive time (or, for an outbound message, send time) is in [from_ns,
  // to_ns); 0 leaves that end open.
  std::int64_t from_ns = 0;
  std::int64_t to_ns = 0;
  bool header = true;  // write the column names first
  // Also the market data of these symbols ("*": every symbol): trades, and with md_books the
  // book tickers and every depth level.
  std::vector<std::string> symbols;
  bool md_books = true;
};

// Writes the rows of every journal in turn to `out`; returns how many. Throws
// std::runtime_error when a journal cannot be read.
std::uint64_t write_order_events(const std::vector<std::string>& journals,
                                 std::FILE* out,
                                 const OrderEventsOptions& opt = {});

}  // namespace fastmm::bt
