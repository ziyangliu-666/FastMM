#pragma once
// Tardis.dev normalized datasets (https://datasets.tardis.dev) as a market-data source. The
// first day of every month is free for every exchange and symbol; the rest needs an API key.
// `python3 -m fastmm.data fetch --source tardis` downloads and gunzips them into the cache.
//
//   incremental_book_L2  exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount
//                        a full book snapshot, then one row per level change (amount 0 deletes)
//   trades               exchange,symbol,timestamp,local_timestamp,id,side,price,amount
//                        side is the aggressor
//
// Unlike Binance's bookTicker this is real L2: a backtest on it knows the queue at any price
// the feed showed, so quoting behind the touch is modelled rather than guessed. Depth is
// truncated to the 256 best levels per side, which is the widest message the simulator moves.
#include "fastmm/backtest/binance_source.hpp"
#include "fastmm/backtest/data_source.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fastmm::bt {

struct TardisSourceConfig {
  std::string dir;       // cache root
  std::string exchange;  // "binance-futures", "binance", "bitmex", ...
  std::string symbol;    // "BTCUSDT"
  std::vector<std::string> dates;
  bool book = true;
  bool trades = true;
  // timestamp (the exchange's own clock) or local_timestamp (when the collector received it).
  bool local_clock = false;
  InstrumentId instrument{0};
  VenueId venue{0};
  Timestamp from{};
  Timestamp to{};
};

// Cache key of one dataset file, mirroring the download URL:
// "tardis/v1/binance-futures/trades/2026/09/01/BTCUSDT.csv".
[[nodiscard]] std::string tardis_key(std::string_view exchange,
                                     std::string_view data_type,
                                     std::string_view symbol,
                                     std::string_view date,
                                     std::string_view ext);

// One normalized CSV, book or trades, decided by `book`. With `from` set, the book rows before it
// build a book that goes out as one snapshot at the first row at or after `from`, so a run that
// starts in the middle of the day starts on the book the file had there.
class TardisFileSource final : public MdSource {
 public:
  TardisFileSource(const std::string& path, const TardisSourceConfig& cfg, bool book);
  const EventHeader* next() override;
  void reset() override;
  [[nodiscard]] Timestamp start_ts() const override { return first_ts_; }
  [[nodiscard]] std::uint64_t rows() const noexcept { return rows_; }

 private:
  bool provide(Row& r);
  void keep(const Row& r);
  bool prime(const Row& first);

  CsvLineReader reader_;
  TardisSourceConfig cfg_;
  bool book_;
  RowAssembler asm_;
  std::int64_t prev_ts_ = 0;
  Timestamp first_ts_{};
  std::uint64_t rows_ = 0;
  bool done_ = false;
  // The book before `from` (raw price -> raw quantity), then the snapshot it becomes: the primer_
  // rows, followed by held_, the first row at or after `from`.
  std::map<std::int64_t, std::int64_t> bids_;
  std::map<std::int64_t, std::int64_t> asks_;
  std::int64_t snapshot_ts_ = 0;
  bool in_snapshot_ = false;
  bool primed_ = false;
  std::vector<Row> primer_;
  std::size_t primer_at_ = 0;
  Row held_{};
  bool have_held_ = false;
};

// Book and trades of one symbol over a range of days, chained by day and merged by time.
class TardisDataset final : public MdSource {
 public:
  // Throws std::runtime_error naming the missing file and the fetch command that gets it.
  explicit TardisDataset(const TardisSourceConfig& cfg);
  ~TardisDataset() override;
  const EventHeader* next() override { return merged_->next(); }
  void reset() override { merged_->reset(); }
  [[nodiscard]] Timestamp start_ts() const override { return merged_->start_ts(); }
  [[nodiscard]] const std::vector<std::string>& files() const noexcept { return files_; }

 private:
  std::vector<std::string> files_;
  std::vector<std::unique_ptr<MdSource>> owned_;
  std::vector<std::unique_ptr<MdSource>> chains_;
  std::unique_ptr<MdSource> merged_;
};

}  // namespace fastmm::bt
