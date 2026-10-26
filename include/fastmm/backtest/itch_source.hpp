#pragma once
// A day of Nasdaq TotalView-ITCH 5.0 as a market-data source: every order the exchange displayed,
// added, executed, cancelled and replaced, in the order its matching engine did it. NASDAQ
// publishes full days at emi.nasdaq.com/ITCH (gunzipped, a 2-byte big-endian length before each
// message).
//
// For each instrument of the configuration on the source's venue (matched by symbol through the
// day's Stock Directory) the source keeps an L3 book (ItchL2Bridge) and yields two streams, merged
// in exchange order:
//
//   * the order-by-order events the venue applies (OrderAddL3, OrderExecL3, OrderCancelL3,
//     OrderReplaceL3). SimTransport keeps them to itself: under fill_model = "l3_queue" they give
//     each of our orders its exact place in its level's FIFO, and the engine never sees them;
//   * the L2 view the engine trades on: one BookDeltaMsg per book for the messages of one ITCH
//     timestamp (the top `depth` levels a side), a TradeMsg for every printable execution, and the
//     P and Q trades flagged TradeMsg::kNoAggressor.
//
// The books build from the start of the file. With `start`, the messages before it are applied
// without being yielded, and at `start` the source yields each book's resting orders in queue
// order (OrderAddL3, so the venue holds the queues it had) and then each book's snapshot.
// Exchange time and receive time are both midnight New York plus the message's timestamp.
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/sim/md_source.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fastmm::bt {

struct ItchSourceConfig {
  std::string path;
  // Trading day YYYY-MM-DD; empty: from a file name that starts MMDDYYYY, as NASDAQ's do.
  std::string date;
  std::vector<std::pair<std::string, InstrumentId>> symbols;
  VenueId venue{0};
  // Nanoseconds after midnight New York; negative: the whole file.
  std::int64_t start_ns = -1;
  std::int64_t end_ns = -1;
  std::uint32_t depth = 64;                       // L2 levels per side the engine gets, 1..256
  std::size_t max_orders = std::size_t{1} << 18;  // resting orders per book
};

// UTC nanoseconds of 00:00 in New York on `date` (YYYY-MM-DD), codecs::nasdaq::eastern_midnight().
// Throws std::runtime_error on a malformed date.
[[nodiscard]] Timestamp new_york_midnight(std::string_view date);

// "HH:MM[:SS]" -> nanoseconds after midnight. Throws std::runtime_error.
[[nodiscard]] std::int64_t parse_time_of_day(std::string_view s);

class ItchSource final : public sim::MdSource {
 public:
  // Throws std::runtime_error when the file cannot be read, the day cannot be told or no symbol is
  // configured.
  explicit ItchSource(ItchSourceConfig cfg);
  ~ItchSource() override;
  ItchSource(const ItchSource&) = delete;
  ItchSource& operator=(const ItchSource&) = delete;

  const EventHeader* next() override;
  void reset() override;
  [[nodiscard]] Timestamp start_ts() const override { return first_ts_; }
  [[nodiscard]] std::string note() const override;
  [[nodiscard]] bool order_by_order() const override { return true; }

 private:
  struct State;
  void begin();
  bool step();  // applies the next message; false at the end
  void open_books(std::int64_t ts);

  ItchSourceConfig cfg_;
  const unsigned char* data_ = nullptr;
  std::size_t size_ = 0;
  Timestamp midnight_{};
  Timestamp first_ts_{};
  std::unique_ptr<State> st_;
};

}  // namespace fastmm::bt
