#pragma once
// Binance public data dumps (data.binance.vision) as a market-data source. The files are the
// daily ZIPs of CSV the archive publishes per symbol; `python -m fastmm.data fetch` downloads
// and unpacks them into a cache that mirrors the bucket layout, and this reads the CSVs.
//
//   bookTicker   USDⓈ-M futures only, 2023-05-16 .. 2024-03-30 (the archive stopped there)
//                update_id,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty,
//                transaction_time,event_time
//   aggTrades    spot and USDⓈ-M futures, to yesterday
//                agg_trade_id,price,quantity,first_trade_id,last_trade_id,transact_time,
//                is_buyer_maker[,is_best_match]
//
// bookTicker is the top of book and nothing else, so the book this builds is one level deep
// per side: BookSnapshot for the first update, then BookDelta carrying only what changed (a
// price move is the old level deleted and the new one added). What that costs a backtest is
// in docs/how-to/backtesting/binance-public-data.md; the short version is that an order away
// from the touch is modelled as alone at its price, because the feed never showed the queue
// there.
//
// aggTrades is aggregated: consecutive fills of one aggressing order at one price arrive as a
// single row. The queue model consumes the whole aggregate at once, which is what the venue's
// matching engine did.
#include "fastmm/backtest/data_source.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::bt {

// Buffered line reader over a file too large to hold in memory (a day of BTCUSDT bookTicker is
// about 1.5 GB of text). Lines are views into an internal buffer, valid until the next call.
class CsvLineReader {
 public:
  // Throws std::runtime_error when the file cannot be opened.
  explicit CsvLineReader(const std::string& path);
  ~CsvLineReader();
  CsvLineReader(const CsvLineReader&) = delete;
  CsvLineReader& operator=(const CsvLineReader&) = delete;

  bool next(std::string_view& line);
  void rewind();
  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::uint64_t line_no() const noexcept { return line_no_; }

 private:
  bool fill();

  std::string path_;
  void* file_ = nullptr;  // std::FILE*
  std::vector<char> buf_;
  std::size_t begin_ = 0;  // unconsumed bytes [begin_, end_)
  std::size_t end_ = 0;
  bool eof_ = false;
  std::uint64_t line_no_ = 0;
};

enum class BinanceMarket : std::uint8_t { Spot, UsdmFutures };

// "spot" | "um" | "futures/um"; std::nullopt when the name is none of them.
[[nodiscard]] std::optional<BinanceMarket> parse_binance_market(std::string_view s) noexcept;
// Key of one daily file inside the archive, e.g.
// "data/futures/um/daily/bookTicker/BTCUSDT/BTCUSDT-bookTicker-2024-03-27.csv". The cache the
// fetch command writes mirrors these keys, so the path under the cache root is the same.
[[nodiscard]] std::string binance_daily_key(BinanceMarket market,
                                            std::string_view kind,
                                            std::string_view symbol,
                                            std::string_view date,
                                            std::string_view ext);

struct BinanceSourceConfig {
  std::string dir;     // cache root (the fetch command's --dir)
  std::string symbol;  // "BTCUSDT"
  std::vector<std::string> dates;
  BinanceMarket market = BinanceMarket::UsdmFutures;
  bool book = true;    // read bookTicker
  bool trades = true;  // read aggTrades
  // bookTicker carries two clocks: transaction_time is when the matching engine changed the
  // book, event_time when the stream published it. The default aligns with aggTrades, whose
  // only clock is the transaction time.
  bool event_clock = false;
  InstrumentId instrument{0};
  VenueId venue{0};
  Timestamp from{};  // invalid: from the first event of the first day
  Timestamp to{};    // invalid: to the last event of the last day
};

// One `*-bookTicker-*.csv`, as a one-level-deep book.
class BinanceBookTickerSource final : public MdSource {
 public:
  BinanceBookTickerSource(const std::string& path, const BinanceSourceConfig& cfg);
  const EventHeader* next() override;
  void reset() override;
  [[nodiscard]] Timestamp start_ts() const override { return first_ts_; }
  [[nodiscard]] std::uint64_t rows() const noexcept { return rows_; }
  // Rows whose timestamp went backwards; each is clamped to the previous one.
  [[nodiscard]] std::uint64_t clamped() const noexcept { return clamped_; }

 private:
  CsvLineReader reader_;
  BinanceSourceConfig cfg_;
  sim::EventBuf buf_{};
  Price bid_px_{}, ask_px_{};
  Qty bid_qty_{}, ask_qty_{};
  bool have_book_ = false;
  std::int64_t prev_ts_ = 0;
  Timestamp first_ts_{};
  std::uint64_t rows_ = 0;
  std::uint64_t clamped_ = 0;
};

// One `*-aggTrades-*.csv`.
class BinanceAggTradesSource final : public MdSource {
 public:
  BinanceAggTradesSource(const std::string& path, const BinanceSourceConfig& cfg);
  const EventHeader* next() override;
  void reset() override;
  [[nodiscard]] Timestamp start_ts() const override { return first_ts_; }
  [[nodiscard]] std::uint64_t rows() const noexcept { return rows_; }
  [[nodiscard]] std::uint64_t clamped() const noexcept { return clamped_; }

 private:
  CsvLineReader reader_;
  BinanceSourceConfig cfg_;
  sim::EventBuf buf_{};
  std::int64_t prev_ts_ = 0;
  Timestamp first_ts_{};
  std::uint64_t rows_ = 0;
  std::uint64_t clamped_ = 0;
};

// Trades and book of a symbol over a range of days: one source per file, chained by day and
// merged by time. Trades win a tie, so a trade consumes the queue before the book update that
// records its effect (both clocks are milliseconds, so ties are common).
class BinanceDataset final : public MdSource {
 public:
  // Throws std::runtime_error naming the missing file and the fetch command that gets it.
  explicit BinanceDataset(const BinanceSourceConfig& cfg);
  ~BinanceDataset() override;
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

// Default cache root: $FASTMM_DATA_HOME, else $XDG_CACHE_HOME/fastmm/data, else
// ~/.cache/fastmm/data.
[[nodiscard]] std::string default_data_dir();
// Every date from `first` to `last` inclusive, as "YYYY-MM-DD". Throws std::runtime_error on a
// malformed date or a range that runs backwards.
[[nodiscard]] std::vector<std::string> date_range(std::string_view first, std::string_view last);
// "YYYY-MM-DD[THH:MM[:SS]]", or "HH:MM[:SS]" on `day`, as nanoseconds since the epoch in UTC.
// Throws std::runtime_error on anything else.
[[nodiscard]] Timestamp parse_utc_time(std::string_view s, std::string_view day);

}  // namespace fastmm::bt
