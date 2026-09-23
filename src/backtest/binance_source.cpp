#include "fastmm/backtest/binance_source.hpp"

#include "csv_fields.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace fastmm::bt {

namespace {

constexpr std::size_t kReadChunk = 1U << 20;

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("binance: " + what); }

using csv::parse_epoch_ns;
using csv::parse_u64;
using csv::split;

Price parse_price(std::string_view s, const CsvLineReader& r) {
  const auto p = Price::from_decimal(s);
  if (!p) fail(r.path() + ":" + std::to_string(r.line_no()) + ": bad price '" + std::string(s) +
               "'");
  return *p;
}
Qty parse_qty(std::string_view s, const CsvLineReader& r) {
  const auto q = Qty::from_decimal(s);
  if (!q) fail(r.path() + ":" + std::to_string(r.line_no()) + ": bad quantity '" + std::string(s) +
               "'");
  return *q;
}

void stamp(EventHeader& h, std::int64_t ts, std::uint64_t seq) noexcept {
  h.exch_ts = h.recv_ts = Timestamp{ts};
  h.t0_cycles = Cycles{static_cast<std::uint64_t>(ts)};
  h.venue_seq = seq;
}

// Days since the civil epoch (Howard Hinnant's days_from_civil).
std::int64_t days_from_civil(int y, unsigned m, unsigned d) noexcept {
  y -= static_cast<int>(m) <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153U * (m + (m > 2 ? -3U : 9U)) + 2U) / 5U + d - 1U;
  const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
  return era * 146'097 + static_cast<std::int64_t>(doe) - 719'468;
}

void civil_from_days(std::int64_t z, int& y, unsigned& m, unsigned& d) noexcept {
  z += 719'468;
  const std::int64_t era = (z >= 0 ? z : z - 146'096) / 146'097;
  const auto doe = static_cast<unsigned>(z - era * 146'097);
  const unsigned yoe = (doe - doe / 1460U + doe / 36'524U - doe / 146'096U) / 365U;
  const std::int64_t yr = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
  const unsigned mp = (5U * doy + 2U) / 153U;
  d = doy - (153U * mp + 2U) / 5U + 1U;
  m = mp + (mp < 10U ? 3U : -9U);
  y = static_cast<int>(yr + (m <= 2U ? 1 : 0));
}

std::int64_t parse_date(std::string_view s) {
  std::uint64_t y = 0;
  std::uint64_t m = 0;
  std::uint64_t d = 0;
  if (s.size() != 10 || s[4] != '-' || s[7] != '-' || !parse_u64(s.substr(0, 4), y) ||
      !parse_u64(s.substr(5, 2), m) || !parse_u64(s.substr(8, 2), d) || m < 1 || m > 12 || d < 1 ||
      d > 31) {
    fail("'" + std::string(s) + "' is not a YYYY-MM-DD date");
  }
  return days_from_civil(static_cast<int>(y), static_cast<unsigned>(m), static_cast<unsigned>(d));
}

std::string format_date(std::int64_t days) {
  int y = 0;
  unsigned m = 0;
  unsigned d = 0;
  civil_from_days(days, y, m, d);
  char out[11];
  std::snprintf(out, sizeof(out), "%04d-%02u-%02u", y, m, d);
  return {out, 10};
}

}  // namespace

// ---- CsvLineReader ---------------------------------------------------------------------------

CsvLineReader::CsvLineReader(const std::string& path) : path_(path), buf_(kReadChunk) {
  file_ = std::fopen(path.c_str(), "rb");
  if (file_ == nullptr) fail("cannot open " + path);
}

CsvLineReader::~CsvLineReader() {
  if (file_ != nullptr) std::fclose(static_cast<std::FILE*>(file_));
}

bool CsvLineReader::fill() {
  if (eof_) return false;
  // Move the partial line to the front, grow if one line fills the whole buffer.
  if (begin_ > 0) {
    std::memmove(buf_.data(), buf_.data() + begin_, end_ - begin_);
    end_ -= begin_;
    begin_ = 0;
  }
  if (end_ == buf_.size()) buf_.resize(buf_.size() * 2);
  const std::size_t n =
      std::fread(buf_.data() + end_, 1, buf_.size() - end_, static_cast<std::FILE*>(file_));
  end_ += n;
  if (n == 0) eof_ = true;
  return n > 0;
}

bool CsvLineReader::next(std::string_view& line) {
  for (;;) {
    const char* base = buf_.data();
    const void* nl = std::memchr(base + begin_, '\n', end_ - begin_);
    if (nl != nullptr) {
      const auto pos = static_cast<std::size_t>(static_cast<const char*>(nl) - base);
      std::size_t stop = pos;
      if (stop > begin_ && base[stop - 1] == '\r') --stop;
      line = std::string_view(base + begin_, stop - begin_);
      begin_ = pos + 1;
      ++line_no_;
      return true;
    }
    if (!fill()) {
      if (begin_ == end_) return false;
      line = std::string_view(buf_.data() + begin_, end_ - begin_);
      begin_ = end_;
      ++line_no_;
      return true;
    }
  }
}

void CsvLineReader::rewind() {
  if (std::fseek(static_cast<std::FILE*>(file_), 0, SEEK_SET) != 0) fail("cannot rewind " + path_);
  begin_ = end_ = 0;
  eof_ = false;
  line_no_ = 0;
}

// ---- paths -----------------------------------------------------------------------------------

std::optional<BinanceMarket> parse_binance_market(std::string_view s) noexcept {
  if (s == "spot") return BinanceMarket::Spot;
  if (s == "um" || s == "futures/um" || s == "usdm") return BinanceMarket::UsdmFutures;
  return std::nullopt;
}

std::string binance_daily_key(BinanceMarket market,
                              std::string_view kind,
                              std::string_view symbol,
                              std::string_view date,
                              std::string_view ext) {
  const std::string_view prefix =
      market == BinanceMarket::Spot ? "data/spot/daily/" : "data/futures/um/daily/";
  std::string out;
  out.reserve(96);
  out.append(prefix).append(kind).append("/").append(symbol).append("/");
  out.append(symbol).append("-").append(kind).append("-").append(date).append(ext);
  return out;
}

std::string default_data_dir() {
  if (const char* e = std::getenv("FASTMM_DATA_HOME"); e != nullptr && *e != '\0') return e;
  if (const char* x = std::getenv("XDG_CACHE_HOME"); x != nullptr && *x != '\0')
    return std::string(x) + "/fastmm/data";
  if (const char* h = std::getenv("HOME"); h != nullptr && *h != '\0')
    return std::string(h) + "/.cache/fastmm/data";
  return ".fastmm-data";
}

std::vector<std::string> date_range(std::string_view first, std::string_view last) {
  const std::int64_t a = parse_date(first);
  const std::int64_t b = parse_date(last);
  if (b < a) fail("date range runs backwards: " + std::string(first) + " .. " + std::string(last));
  if (b - a > 366) fail("date range is longer than a year");
  std::vector<std::string> out;
  out.reserve(static_cast<std::size_t>(b - a + 1));
  for (std::int64_t d = a; d <= b; ++d) out.push_back(format_date(d));
  return out;
}

Timestamp parse_utc_time(std::string_view s, std::string_view day) {
  std::string_view date = day;
  std::string_view clock = s;
  if (s.size() >= 10 && s[4] == '-') {
    date = s.substr(0, 10);
    clock = s.size() > 10 ? s.substr(11) : std::string_view{};
  }
  std::int64_t ns = parse_date(date) * 86'400 * 1'000'000'000;
  if (!clock.empty()) {
    std::uint64_t h = 0;
    std::uint64_t m = 0;
    std::uint64_t sec = 0;
    const bool ok = clock.size() >= 5 && clock[2] == ':' && parse_u64(clock.substr(0, 2), h) &&
                    parse_u64(clock.substr(3, 2), m) &&
                    (clock.size() == 5 ||
                     (clock.size() == 8 && clock[5] == ':' && parse_u64(clock.substr(6, 2), sec)));
    if (!ok || h > 23 || m > 59 || sec > 59)
      fail("'" + std::string(s) + "' is not HH:MM[:SS] or YYYY-MM-DD[THH:MM[:SS]]");
    ns += (static_cast<std::int64_t>(h) * 3600 + static_cast<std::int64_t>(m) * 60 +
           static_cast<std::int64_t>(sec)) *
          1'000'000'000;
  }
  return Timestamp{ns};
}

// ---- bookTicker ------------------------------------------------------------------------------

BinanceBookTickerSource::BinanceBookTickerSource(const std::string& path,
                                                 const BinanceSourceConfig& cfg)
    : reader_(path), cfg_(cfg) {
  if (const EventHeader* h = next()) first_ts_ = h->exch_ts;
  reset();
}

void BinanceBookTickerSource::reset() {
  reader_.rewind();
  have_book_ = false;
  prev_ts_ = 0;
  rows_ = 0;
  clamped_ = 0;
}

const EventHeader* BinanceBookTickerSource::next() {
  std::string_view f[7];
  for (;;) {
    std::string_view line;
    if (!reader_.next(line)) return nullptr;
    if (line.empty()) continue;
    const std::size_t n = split(line, f, 7);
    if (n < 6) fail(reader_.path() + ":" + std::to_string(reader_.line_no()) +
                    ": expected at least 6 fields, got " + std::to_string(n));
    std::int64_t ts = 0;
    const std::string_view clock = cfg_.event_clock && n >= 7 ? f[6] : f[5];
    if (!parse_epoch_ns(clock, ts)) {
      if (reader_.line_no() == 1) continue;  // header
      fail(reader_.path() + ":" + std::to_string(reader_.line_no()) + ": bad timestamp '" +
           std::string(clock) + "'");
    }
    if (ts < prev_ts_) {
      ts = prev_ts_;
      ++clamped_;
    }
    prev_ts_ = ts;
    if (cfg_.from.valid() && ts < cfg_.from.ns) continue;
    if (cfg_.to.valid() && ts > cfg_.to.ns) return nullptr;
    ++rows_;

    std::uint64_t seq = 0;
    static_cast<void>(parse_u64(f[0], seq));
    const Price bid_px = parse_price(f[1], reader_);
    const Qty bid_qty = parse_qty(f[2], reader_);
    const Price ask_px = parse_price(f[3], reader_);
    const Qty ask_qty = parse_qty(f[4], reader_);

    Level bids[2];
    Level asks[2];
    std::uint32_t nb = 0;
    std::uint32_t na = 0;
    const bool snapshot = !have_book_;
    if (snapshot) {
      bids[nb++] = Level{bid_px, bid_qty};
      asks[na++] = Level{ask_px, ask_qty};
    } else {
      if (bid_px != bid_px_) {
        bids[nb++] = Level{bid_px_, Qty{}};  // the old touch is gone
        bids[nb++] = Level{bid_px, bid_qty};
      } else if (bid_qty != bid_qty_) {
        bids[nb++] = Level{bid_px, bid_qty};
      }
      if (ask_px != ask_px_) {
        asks[na++] = Level{ask_px_, Qty{}};
        asks[na++] = Level{ask_px, ask_qty};
      } else if (ask_qty != ask_qty_) {
        asks[na++] = Level{ask_px, ask_qty};
      }
      if (nb == 0 && na == 0) continue;  // a new update id with the same top of book
    }
    bid_px_ = bid_px;
    bid_qty_ = bid_qty;
    ask_px_ = ask_px;
    ask_qty_ = ask_qty;
    have_book_ = true;

    auto& d = buf_.as<BookDeltaMsg>();
    init_header(d,
                snapshot ? EventType::BookSnapshot : EventType::BookDelta,
                cfg_.instrument,
                cfg_.venue,
                BookDeltaMsg::size_for(nb, na));
    if (snapshot) d.hdr.flags |= EventHeader::kSnapshot;
    stamp(d.hdr, ts, seq);
    d.bid_count = nb;
    d.ask_count = na;
    d.first_update_id = d.last_update_id = seq;
    d.prev_update_id = 0;
    std::memcpy(d.levels(), bids, nb * sizeof(Level));
    std::memcpy(d.levels() + nb, asks, na * sizeof(Level));
    return &d.hdr;
  }
}

// ---- aggTrades -------------------------------------------------------------------------------

BinanceAggTradesSource::BinanceAggTradesSource(const std::string& path,
                                               const BinanceSourceConfig& cfg)
    : reader_(path), cfg_(cfg) {
  if (const EventHeader* h = next()) first_ts_ = h->exch_ts;
  reset();
}

void BinanceAggTradesSource::reset() {
  reader_.rewind();
  prev_ts_ = 0;
  rows_ = 0;
  clamped_ = 0;
}

const EventHeader* BinanceAggTradesSource::next() {
  std::string_view f[8];
  for (;;) {
    std::string_view line;
    if (!reader_.next(line)) return nullptr;
    if (line.empty()) continue;
    const std::size_t n = split(line, f, 8);
    if (n < 7) fail(reader_.path() + ":" + std::to_string(reader_.line_no()) +
                    ": expected at least 7 fields, got " + std::to_string(n));
    std::int64_t ts = 0;
    if (!parse_epoch_ns(f[5], ts)) {
      if (reader_.line_no() == 1) continue;  // header
      fail(reader_.path() + ":" + std::to_string(reader_.line_no()) + ": bad timestamp '" +
           std::string(f[5]) + "'");
    }
    if (ts < prev_ts_) {
      ts = prev_ts_;
      ++clamped_;
    }
    prev_ts_ = ts;
    if (cfg_.from.valid() && ts < cfg_.from.ns) continue;
    if (cfg_.to.valid() && ts > cfg_.to.ns) return nullptr;
    ++rows_;

    std::uint64_t id = 0;
    static_cast<void>(parse_u64(f[0], id));
    // is_buyer_maker: the resting side was the bid, so the aggressor sold.
    const bool buyer_maker = f[6] == "true" || f[6] == "True" || f[6] == "1";
    auto& t = buf_.as<TradeMsg>();
    init_header(t, EventType::Trade, cfg_.instrument, cfg_.venue);
    stamp(t.hdr, ts, id);
    t.price = parse_price(f[1], reader_);
    t.qty = parse_qty(f[2], reader_);
    t.trade_id = id;
    t.aggressor = buyer_maker ? Side::Sell : Side::Buy;
    return &t.hdr;
  }
}

// ---- dataset ---------------------------------------------------------------------------------

BinanceDataset::BinanceDataset(const BinanceSourceConfig& cfg) {
  if (cfg.symbol.empty()) fail("no symbol");
  if (cfg.dates.empty()) fail("no dates");
  if (!cfg.book && !cfg.trades) fail("book=false and trades=false leave nothing to read");
  if (cfg.book && cfg.market == BinanceMarket::Spot)
    fail("the archive publishes no spot book data; use market=um, or book=false for trades only");

  auto open_kind = [&](std::string_view kind, bool is_book) {
    std::vector<MdSource*> chain;
    for (const std::string& date : cfg.dates) {
      const std::string path =
          cfg.dir + "/" + binance_daily_key(cfg.market, kind, cfg.symbol, date, ".csv");
      if (!std::filesystem::exists(path)) {
        std::string msg = "no ";
        msg.append(kind).append(" for ").append(cfg.symbol).append(" on ").append(date);
        msg.append(" at ").append(path);
        msg.append("\n  fetch it with: python3 -m fastmm.data fetch --symbol ");
        msg.append(cfg.symbol).append(" --date ").append(date);
        fail(msg);
      }
      owned_.push_back(is_book
                           ? std::unique_ptr<MdSource>(new BinanceBookTickerSource(path, cfg))
                           : std::unique_ptr<MdSource>(new BinanceAggTradesSource(path, cfg)));
      chain.push_back(owned_.back().get());
      files_.push_back(path);
    }
    chains_.push_back(std::make_unique<ChainSource>(std::move(chain)));
  };

  // Trades first: MergedSource keeps the lower index on a tie, and both clocks are
  // milliseconds, so a trade and the book update it caused routinely share a timestamp. The
  // trade has to consume the queue before the book records that the level shrank.
  if (cfg.trades) open_kind("aggTrades", false);
  if (cfg.book) open_kind("bookTicker", true);
  std::vector<MdSource*> parts;
  parts.reserve(chains_.size());
  for (const std::unique_ptr<MdSource>& c : chains_) parts.push_back(c.get());
  merged_ = std::make_unique<MergedSource>(std::move(parts));
}

BinanceDataset::~BinanceDataset() = default;

}  // namespace fastmm::bt
