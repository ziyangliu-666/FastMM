#pragma once
// BinanceUsdmMdFeed: the MarketDataFeed (feed.hpp) for Binance USDⓈ-M futures. Owns the decoder,
// one depth sync per subscribed instrument and the market-data EventSink; runs on the venue's
// reactor thread and does not allocate after add_instrument().
//
// Two combined-stream connections, following the 2026-03-05 URL split ("Important WebSocket Change
// Notice"; legacy /ws and /stream URLs were decommissioned on 2026-04-23):
//   <root>/public/stream?streams=<sym>@depth@100ms/<sym>@bookTicker/...
//   <root>/market/stream?streams=<sym>@aggTrade/...
// Both feed on_message(); only the public connection starts and stops the book syncs.
//
// Book sync ("How to manage a local order book correctly", USDⓈ-M): buffer deltas, GET
// /fapi/v1/depth, drop u < lastUpdateId, the first applied delta has U <= lastUpdateId <= u, and
// every later delta's pu equals the previous u (BinanceFuturesSyncTraits in book_syncer.hpp).
#include "fastmm/core/time.hpp"
#include "fastmm/venues/binance/binance_depth_sync.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_md_parser.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/symbology.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fastmm::venues::binance_usdm {

using binance::SnapshotRequester;
using UsdmDepthSync = binance::BasicBinanceDepthSync<BinanceFuturesSyncTraits>;

struct MdFeedStats {
  std::uint64_t messages = 0;
  std::uint64_t pushed = 0;
  std::uint64_t dropped = 0;  // ring overflow on ticker/trade (deltas resync instead)
  std::uint64_t malformed = 0;
  std::uint64_t ignored = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t snapshots_ok = 0;
  std::uint64_t snapshots_failed = 0;
};

class BinanceUsdmMdFeed {
 public:
  BinanceUsdmMdFeed(const SymbolTable& symbols,
                    VenueId venue,
                    EventSink& sink,
                    SnapshotRequester requester,
                    std::int64_t min_snapshot_interval_ns = UsdmDepthSync::kDefaultMinInterval)
      : symbols_(symbols),
        venue_(venue),
        sink_(sink),
        requester_(requester),
        min_interval_(min_snapshot_interval_ns),
        parser_(symbols, venue) {
    index_.fill(-1);
  }

  // Startup only.
  bool add_instrument(InstrumentId id) {
    if (!symbols_.contains(id) || id.value >= kMaxInstruments || index_[id.value] >= 0)
      return false;
    index_[id.value] = static_cast<std::int16_t>(syncs_.size());
    syncs_.push_back(std::make_unique<UsdmDepthSync>(id, venue_, sink_, requester_, min_interval_));
    ids_.push_back(id);
    return true;
  }
  [[nodiscard]] std::span<const InstrumentId> instruments() const noexcept { return ids_; }

  // "/public/stream?streams=btcusdt@depth@100ms/btcusdt@bookTicker"
  [[nodiscard]] std::string public_target() const {
    return target("/public/stream", {"@depth@100ms", "@bookTicker"});
  }
  // "/market/stream?streams=btcusdt@aggTrade"
  [[nodiscard]] std::string market_target() const {
    return target("/market/stream", {"@aggTrade"});
  }

  // ---- MarketDataFeed ----------------------------------------------------------------------
  ParseStatus on_message(std::string_view json, std::int64_t rx_ts) noexcept {
    ++stats_.messages;
    const Cycles t0 = rdtscp();
    const Timestamp recv = wall_now();
    const DecodeResult r = parser_.decode(json, recv, t0, scratch_);
    switch (r.status) {
      case ParseStatus::Ok:
        break;
      case ParseStatus::Malformed:
        ++stats_.malformed;
        return r.status;
      case ParseStatus::UnknownSymbol:
        ++stats_.unknown_symbol;
        return r.status;
      default:
        ++stats_.ignored;
        return r.status;
    }
    auto* h = reinterpret_cast<EventHeader*>(scratch_);
    h->t1_delta = static_cast<std::uint32_t>(rdtscp() - t0);
    if (r.kind == MdKind::BookDelta) {
      UsdmDepthSync* s = sync(h->instrument);
      if (s == nullptr) return ParseStatus::UnknownSymbol;
      s->on_delta(*reinterpret_cast<const BookDeltaMsg*>(scratch_), rx_ts);
      ++stats_.pushed;
      return ParseStatus::Ok;
    }
    if (!sink_.push(*h)) {
      ++stats_.dropped;
      return ParseStatus::Overflow;
    }
    ++stats_.pushed;
    return ParseStatus::Ok;
  }
  void on_connected() noexcept {
    const std::int64_t now = steady_now().ns;
    for (auto& s : syncs_) s->start(now);
  }
  void on_disconnected() noexcept {
    for (auto& s : syncs_) s->stop();
  }
  [[nodiscard]] std::span<const std::string> subscription_payloads() const noexcept {
    return {};  // streams are selected in the URL
  }

  // ---- REST snapshot plumbing ---------------------------------------------------------------
  void on_snapshot_body(InstrumentId id, std::string_view json, std::int64_t now_ns) noexcept {
    UsdmDepthSync* s = sync(id);
    if (s == nullptr) return;
    const Cycles t0 = rdtscp();
    const DecodeResult r = parser_.decode_depth_snapshot(json, id, wall_now(), t0, scratch_);
    if (r.status != ParseStatus::Ok) {
      ++stats_.snapshots_failed;
      s->on_snapshot_failed(now_ns);
      return;
    }
    auto* h = reinterpret_cast<EventHeader*>(scratch_);
    h->t1_delta = static_cast<std::uint32_t>(rdtscp() - t0);
    ++stats_.snapshots_ok;
    s->on_snapshot(*reinterpret_cast<const BookDeltaMsg*>(scratch_), now_ns);
  }
  void on_snapshot_failed(InstrumentId id, std::int64_t now_ns) noexcept {
    ++stats_.snapshots_failed;
    if (UsdmDepthSync* s = sync(id)) s->on_snapshot_failed(now_ns);
  }
  void on_timer(std::int64_t now_ns) noexcept {
    for (auto& s : syncs_) s->on_timer(now_ns);
  }

  [[nodiscard]] UsdmDepthSync* sync(InstrumentId id) noexcept {
    if (id.value >= kMaxInstruments || index_[id.value] < 0) return nullptr;
    return syncs_[static_cast<std::size_t>(index_[id.value])].get();
  }
  [[nodiscard]] std::uint32_t synced_count() const noexcept {
    std::uint32_t n = 0;
    for (const auto& s : syncs_) n += s->synced() ? 1U : 0U;
    return n;
  }
  [[nodiscard]] std::uint64_t resync_count() const noexcept {
    std::uint64_t n = 0;
    for (const auto& s : syncs_) n += s->resync_count();
    return n;
  }
  [[nodiscard]] const MdFeedStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const MdParserStats& parser_stats() const noexcept { return parser_.stats(); }

 private:
  [[nodiscard]] std::string target(std::string_view path,
                                   std::initializer_list<const char*> suffixes) const {
    std::string t(path);
    t += "?streams=";
    bool first = true;
    for (InstrumentId id : ids_) {
      const std::string sym(symbols_.lower_symbol(id));
      for (const char* suffix : suffixes) {
        if (!first) t += '/';
        first = false;
        t += sym;
        t += suffix;
      }
    }
    return t;
  }

  const SymbolTable& symbols_;
  VenueId venue_;
  EventSink& sink_;
  SnapshotRequester requester_;
  std::int64_t min_interval_;
  BinanceUsdmMdParser parser_;
  std::array<std::int16_t, kMaxInstruments> index_{};
  std::vector<std::unique_ptr<UsdmDepthSync>> syncs_;
  std::vector<InstrumentId> ids_;
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
  MdFeedStats stats_;
};

static_assert(MarketDataFeed<BinanceUsdmMdFeed>);

}  // namespace fastmm::venues::binance_usdm
