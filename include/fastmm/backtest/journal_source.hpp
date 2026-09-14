#pragma once
// JournalSource: market-data events of an .fmj journal (BookDelta / BookSnapshot / Trade /
// BookTicker records, in sequence order). Everything else the journal holds (order events,
// timers, outbound copies) is skipped: the backtest re-derives those.
#include "fastmm/backtest/data_source.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/journal.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace fastmm::bt {

class JournalSource final : public MdSource {
 public:
  // Throws std::runtime_error if the file cannot be opened/validated.
  explicit JournalSource(const std::string& path);
  const EventHeader* next() override;
  void reset() override;
  [[nodiscard]] Timestamp start_ts() const override { return first_ts_; }
  [[nodiscard]] const JournalReader& reader() const noexcept { return reader_; }
  [[nodiscard]] std::size_t md_events() const noexcept { return md_events_; }

  [[nodiscard]] static bool is_market_data(EventType t) noexcept {
    return t == EventType::BookDelta || t == EventType::BookSnapshot || t == EventType::Trade ||
           t == EventType::BookTicker || t == EventType::OptionTicker;
  }

 private:
  JournalReader reader_;
  Timestamp first_ts_{};
  std::size_t md_events_ = 0;
};

// Writes the events of `source` (all, or the first `max_events` when non-zero) to a fresh
// .fmj with JournalFileWriter (instrument table + header stamped with `seed`); used to
// produce fixtures. Returns the number of events written.
std::uint64_t write_md_journal(MdSource& source,
                               const std::string& path,
                               const InstrumentTable& instruments,
                               std::uint64_t seed,
                               std::string_view strategy = "data",
                               std::uint64_t max_events = 0);

}  // namespace fastmm::bt
