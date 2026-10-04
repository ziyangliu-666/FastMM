#pragma once
// JournalSource: market-data events of an .fmj journal (BookDelta / BookSnapshot / Trade /
// BookTicker records, in sequence order). Everything else the journal holds (order events,
// timers, outbound copies) is skipped: the backtest re-derives those.
//
// parts: a session recorded with [engine] journal_max_bytes is several files, `x.fmj` then
// `x.1.fmj`, `x.2.fmj`, ... (JournalFileWriter::part_path), each a complete journal that repeats
// the prologue. Given `x.fmj`, the source reads the later parts after it, in order, until one is
// missing, as one stream; their prologues are never events, so nothing repeats. A part of another
// session (its session_id differs) is refused. The sequence numbers continue across parts; a part
// whose first does not follow the previous part's last is read anyway, and seq_gaps() / note()
// say so: a part is missing in between, or the previous one was cut short.
//
// strip_own: the journal is a live session's, whose venue feed shows the session's own resting
// orders; take them out of the depth and top of book (OwnOrderStripper, own_orders.hpp) so the
// backtest does not see its live twin as someone else's liquidity. A BookTicker whose best bid or
// ask was only ours is dropped. Public trades are kept, our own fills included: the aggressor
// existed whether or not our order did, and without it would have traded with the next order at
// that price, which is what a simulated order there stands for.
//
// remap: the journal numbers its instruments as the recording session did, so two recordings of
// different instrument sets both count from 0 and, merged, overwrite each other's books. Given the
// backtest configuration's table, every event's instrument id becomes that of the instrument with
// the same symbol there (the recording's venue first, else any venue listing the symbol) and its
// venue that instrument's; an event whose symbol the configuration lacks is dropped, and note()
// counts them. The copy holds up to kMaxMsgBytes: a recorded snapshot can be deeper than an
// EventBuf. Remap applies after strip_own.
#include "fastmm/backtest/data_source.hpp"
#include "fastmm/backtest/own_orders.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/sim/sim_account.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::bt {

class JournalSource final : public MdSource {
 public:
  // Throws std::runtime_error if a file cannot be opened/validated. parts = false reads `path`
  // alone.
  // remap: the configuration's instrument table (see above); null leaves the ids as recorded.
  explicit JournalSource(const std::string& path,
                         bool strip_own = false,
                         bool parts = true,
                         const InstrumentTable* remap = nullptr);
  const EventHeader* next() override;
  void reset() override;
  [[nodiscard]] Timestamp start_ts() const override { return first_ts_; }
  // The first part: its header, instrument table and configuration are every part's.
  [[nodiscard]] const JournalReader& reader() const noexcept { return parts_.front(); }
  // The files read, in order; the first is `path`.
  [[nodiscard]] const std::vector<std::string>& paths() const noexcept { return paths_; }
  // Parts whose first sequence number did not follow the previous part's last.
  [[nodiscard]] std::size_t seq_gaps() const noexcept { return seq_gaps_; }
  [[nodiscard]] std::size_t md_events() const noexcept { return md_events_; }
  // Null unless strip_own.
  [[nodiscard]] const OwnOrderStripper* stripper() const noexcept { return stripper_.get(); }
  // remap: events dropped so far because the configuration lacks their symbol.
  [[nodiscard]] std::size_t dropped() const noexcept { return dropped_; }
  [[nodiscard]] std::string note() const override;
  // The first balance snapshot the journal recorded for each venue (the BalanceMsg rows from its
  // first kSnapshot message to the kSnapshotEnd), each asset at its total; account rows
  // (kAccount) are left out. Empty when the journal has none.
  [[nodiscard]] const std::vector<sim::SimAccountConfig>* balance_snapshots() const override {
    return &balances_;
  }

  [[nodiscard]] static bool is_market_data(EventType t) noexcept {
    return t == EventType::BookDelta || t == EventType::BookSnapshot || t == EventType::Trade ||
           t == EventType::BookTicker || t == EventType::OptionTicker || t == EventType::PerpState;
  }

 private:
  void open_part(const std::string& path);

  std::vector<JournalReader> parts_;
  std::vector<std::string> paths_;
  std::size_t at_ = 0;  // the part next() is reading
  std::size_t seq_gaps_ = 0;
  std::string gap_;  // the first gap, for note()
  Timestamp first_ts_{};
  std::size_t md_events_ = 0;
  std::unique_ptr<OwnOrderStripper> stripper_;
  std::size_t orders_ = 0;
  EventBuf buf_;
  std::vector<sim::SimAccountConfig> balances_;
  // remap: per journal instrument id, the configured id (kUnmapped: dropped) and its venue.
  static constexpr std::uint32_t kUnmapped = 0xFFFF'FFFF;
  bool remap_ = false;
  std::vector<std::uint32_t> map_;
  std::vector<VenueId> map_venue_;
  std::size_t unmapped_ = 0;  // journal instruments the configuration lacks
  std::size_t dropped_ = 0;
  std::vector<std::uint64_t> rbuf_;  // the remapped event, a copy of up to kMaxMsgBytes
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
