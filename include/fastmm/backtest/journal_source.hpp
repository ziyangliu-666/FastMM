#pragma once
// JournalSource: market-data events of an .fmj journal (BookDelta / BookSnapshot / Trade /
// BookTicker records, in sequence order). Everything else the journal holds (order events,
// timers, outbound copies) is skipped: the backtest re-derives those.
//
// strip_own: the journal is a live session's, whose venue feed shows the session's own resting
// orders; take them out of the depth and top of book (OwnOrderStripper, own_orders.hpp) so the
// backtest does not see its live twin as someone else's liquidity. A BookTicker whose best bid or
// ask was only ours is dropped. Public trades are kept, our own fills included: the aggressor
// existed whether or not our order did, and without it would have traded with the next order at
// that price, which is what a simulated order there stands for.
#include "fastmm/backtest/data_source.hpp"
#include "fastmm/backtest/own_orders.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/journal.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace fastmm::bt {

class JournalSource final : public MdSource {
 public:
  // Throws std::runtime_error if the file cannot be opened/validated.
  explicit JournalSource(const std::string& path, bool strip_own = false);
  const EventHeader* next() override;
  void reset() override;
  [[nodiscard]] Timestamp start_ts() const override { return first_ts_; }
  [[nodiscard]] const JournalReader& reader() const noexcept { return reader_; }
  [[nodiscard]] std::size_t md_events() const noexcept { return md_events_; }
  // Null unless strip_own.
  [[nodiscard]] const OwnOrderStripper* stripper() const noexcept { return stripper_.get(); }
  [[nodiscard]] std::string note() const override;

  [[nodiscard]] static bool is_market_data(EventType t) noexcept {
    return t == EventType::BookDelta || t == EventType::BookSnapshot || t == EventType::Trade ||
           t == EventType::BookTicker || t == EventType::OptionTicker;
  }

 private:
  JournalReader reader_;
  Timestamp first_ts_{};
  std::size_t md_events_ = 0;
  std::unique_ptr<OwnOrderStripper> stripper_;
  std::size_t orders_ = 0;
  EventBuf buf_;
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
