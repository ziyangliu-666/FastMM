#include "fastmm/backtest/tardis_source.hpp"

#include "csv_fields.hpp"

#include <filesystem>
#include <stdexcept>

namespace fastmm::bt {

namespace {

[[noreturn]] void fail(const std::string& what) {
  throw std::runtime_error("tardis: " + what);
}

using csv::parse_epoch_ns;
using csv::parse_u64;
using csv::split;

}  // namespace

std::string tardis_key(std::string_view exchange,
                       std::string_view data_type,
                       std::string_view symbol,
                       std::string_view date,
                       std::string_view ext) {
  if (date.size() != 10) fail("'" + std::string(date) + "' is not a YYYY-MM-DD date");
  std::string out = "tardis/v1/";
  out.append(exchange).append("/").append(data_type).append("/");
  out.append(date.substr(0, 4)).append("/").append(date.substr(5, 2)).append("/");
  out.append(date.substr(8, 2)).append("/").append(symbol).append(ext);
  return out;
}

// ---- one file --------------------------------------------------------------------------------

TardisFileSource::TardisFileSource(const std::string& path,
                                   const TardisSourceConfig& cfg,
                                   bool book)
    : reader_(path), cfg_(cfg), book_(book), asm_(cfg.venue) {
  if (const EventHeader* h = next()) first_ts_ = h->exch_ts;
  reset();
}

void TardisFileSource::reset() {
  reader_.rewind();
  asm_.reset();
  prev_ts_ = 0;
  rows_ = 0;
  done_ = false;
  bids_.clear();
  asks_.clear();
  in_snapshot_ = false;
  primed_ = false;
  primer_.clear();
  primer_at_ = 0;
  have_held_ = false;
}

// A row before `from` updates the book kept for the first message at or after it. The rows of a
// snapshot share one timestamp, and the first of them replaces what was there.
void TardisFileSource::keep(const Row& r) {
  if (r.type == RowType::Snapshot) {
    if (!in_snapshot_ || r.ts != snapshot_ts_) {
      bids_.clear();
      asks_.clear();
    }
    in_snapshot_ = true;
    snapshot_ts_ = r.ts;
  } else {
    in_snapshot_ = false;
  }
  auto& side = r.side == Side::Buy ? bids_ : asks_;
  if (r.qty.is_positive()) {
    side[r.price.raw] = r.qty.raw;
  } else {
    side.erase(r.price.raw);
  }
}

// The book kept from before `from` goes out as a snapshot stamped with the first row at or after
// it, which follows it; nothing when that row starts a snapshot itself. True when there is one.
bool TardisFileSource::prime(const Row& first) {
  primed_ = true;
  if (first.type != RowType::Snapshot && (!bids_.empty() || !asks_.empty())) {
    Row r = first;
    r.type = RowType::Snapshot;
    r.seq = 0;
    r.side = Side::Buy;
    for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {  // best first
      r.price = Price::from_raw(it->first);
      r.qty = Qty::from_raw(it->second);
      primer_.push_back(r);
    }
    r.side = Side::Sell;
    for (const auto& [p, q] : asks_) {
      r.price = Price::from_raw(p);
      r.qty = Qty::from_raw(q);
      primer_.push_back(r);
    }
    held_ = first;
    have_held_ = true;
  }
  bids_.clear();
  asks_.clear();
  return have_held_;
}

// Book rows:   exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount
// Trade rows:  exchange,symbol,timestamp,local_timestamp,id,side,price,amount
// The two layouts differ only in what field 4 means, so one parser reads both.
bool TardisFileSource::provide(Row& r) {
  if (primer_at_ < primer_.size()) {
    r = primer_[primer_at_++];
    return true;
  }
  if (have_held_) {
    have_held_ = false;
    r = held_;
    return true;
  }
  std::string_view f[8];
  while (!done_) {
    std::string_view line;
    if (!reader_.next(line)) return false;
    if (line.empty()) continue;
    const std::size_t n = split(line, f, 8);
    if (n < 8)
      fail(reader_.path() + ":" + std::to_string(reader_.line_no()) + ": expected 8 fields, got " +
           std::to_string(n));
    std::int64_t ts = 0;
    const std::string_view ts_field = f[cfg_.local_clock ? 3 : 2];
    if (!parse_epoch_ns(ts_field, ts)) {
      if (reader_.line_no() == 1) continue;  // header
      fail(reader_.path() + ":" + std::to_string(reader_.line_no()) + ": bad timestamp '" +
           std::string(ts_field) + "'");
    }
    if (ts < prev_ts_) ts = prev_ts_;
    prev_ts_ = ts;
    const bool before = cfg_.from.valid() && ts < cfg_.from.ns;
    if (before && !book_) continue;
    if (cfg_.to.valid() && ts > cfg_.to.ns) {
      done_ = true;
      return false;
    }
    const auto px = Price::from_decimal(f[6]);
    const auto qty = Qty::from_decimal(f[7]);
    if (!px || !qty)
      fail(reader_.path() + ":" + std::to_string(reader_.line_no()) + ": bad price or amount");
    r.ts = ts;
    r.inst = cfg_.instrument.value;
    r.price = *px;
    r.qty = *qty;
    if (book_) {
      r.type = f[4] == "true" ? RowType::Snapshot : RowType::Delta;
      r.side = f[5] == "bid" ? Side::Buy : Side::Sell;
      r.seq = 0;  // the feed has no update id: rows of one timestamp form one message
      if (before) {
        keep(r);
        continue;
      }
    } else {
      r.type = RowType::Trade;
      r.side = f[5] == "buy" ? Side::Buy : Side::Sell;  // aggressor
      std::uint64_t id = 0;
      static_cast<void>(parse_u64(f[4], id));
      r.seq = id;
    }
    ++rows_;
    if (book_ && !primed_ && prime(r)) r = primer_[primer_at_++];
    return true;
  }
  return false;
}

const EventHeader* TardisFileSource::next() {
  return asm_.next([this](Row& r) { return provide(r); });
}

// ---- dataset ---------------------------------------------------------------------------------

TardisDataset::TardisDataset(const TardisSourceConfig& cfg) {
  if (cfg.exchange.empty()) fail("no exchange");
  if (cfg.symbol.empty()) fail("no symbol");
  if (cfg.dates.empty()) fail("no dates");
  if (!cfg.book && !cfg.trades) fail("book=false and trades=false leave nothing to read");

  auto open_kind = [&](std::string_view data_type, bool book) {
    std::vector<MdSource*> chain;
    for (const std::string& date : cfg.dates) {
      const std::string path =
          cfg.dir + "/" + tardis_key(cfg.exchange, data_type, cfg.symbol, date, ".csv");
      if (!std::filesystem::exists(path)) {
        std::string msg = "no ";
        msg.append(data_type).append(" for ").append(cfg.exchange).append(" ").append(cfg.symbol);
        msg.append(" on ").append(date).append(" at ").append(path);
        msg.append("\n  fetch it with: python3 -m fastmm.data fetch --source tardis --exchange ");
        msg.append(cfg.exchange).append(" --symbol ").append(cfg.symbol);
        msg.append(" --date ").append(date);
        fail(msg);
      }
      owned_.push_back(std::make_unique<TardisFileSource>(path, cfg, book));
      chain.push_back(owned_.back().get());
      files_.push_back(path);
    }
    chains_.push_back(std::make_unique<ChainSource>(std::move(chain)));
  };

  // Trades first, for the same reason as the Binance dataset: on a tie the trade has to
  // consume the queue before the book records that the level shrank.
  if (cfg.trades) open_kind("trades", false);
  if (cfg.book) open_kind("incremental_book_L2", true);
  std::vector<MdSource*> parts;
  parts.reserve(chains_.size());
  for (const std::unique_ptr<MdSource>& c : chains_) parts.push_back(c.get());
  merged_ = std::make_unique<MergedSource>(std::move(parts));
}

TardisDataset::~TardisDataset() = default;

}  // namespace fastmm::bt
