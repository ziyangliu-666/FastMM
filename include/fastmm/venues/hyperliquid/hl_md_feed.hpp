#pragma once
// HlMdFeed: the MarketDataFeed (feed.hpp) for Hyperliquid. Owns the decoder, one book sync per
// instrument and the market-data EventSink; runs on the venue's reactor thread.
//
// Subscriptions, one per coin and channel ({"method":"subscribe","subscription":{"type":..,
// "coin":"xyz:NVDA"}}): l2Book (the book, 20 levels a side, every push a whole book), bbo (the
// touch, pushed when it changes) and trades, each switchable. Every l2Book push is a snapshot, so
// the sync never sees a gap; it still re-subscribes a coin whose book has not come within its
// snapshot timeout, or whose push overflowed the ring.
#include "fastmm/core/time.hpp"
#include "fastmm/venues/book_sync.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/hyperliquid/hl_md_parser.hpp"
#include "fastmm/venues/hyperliquid/hl_order_encoder.hpp"
#include "fastmm/venues/symbology.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace fastmm::venues::hyperliquid {

// Every l2Book push is a whole book (kSnapshot): no deltas, no gaps.
struct HlBookTraits {
  static constexpr bool kNeedsRestSnapshot = false;
  static constexpr bool kBuffersDeltas = false;
  static constexpr bool is_stale(const BookDeltaMsg&, std::uint64_t) noexcept { return false; }
  static constexpr bool first_applies(const BookDeltaMsg&, std::uint64_t) noexcept { return true; }
  static constexpr bool next_applies(const BookDeltaMsg&, std::uint64_t) noexcept { return true; }
  static constexpr bool is_snapshot_marker(const BookDeltaMsg&) noexcept { return false; }
};
using HlBookSync = StreamBookSync<HlBookTraits>;

struct MdFeedStats {
  std::uint64_t messages = 0;
  std::uint64_t pushed = 0;
  std::uint64_t dropped = 0;
  std::uint64_t malformed = 0;
  std::uint64_t ignored = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t errors = 0;
  std::uint64_t pongs = 0;
};

struct MdChannels {
  bool l2_book = true;
  bool bbo = true;
  bool trades = true;
};

class HlMdFeed {
 public:
  HlMdFeed(const SymbolTable& symbols,
           VenueId venue,
           EventSink& sink,
           InstrumentCallback requester,
           MdChannels channels = {},
           std::string dex = {},
           std::int64_t min_resubscribe_interval_ns = HlBookSync::kDefaultMinInterval)
      : symbols_(symbols),
        venue_(venue),
        sink_(sink),
        requester_(requester),
        channels_(channels),
        min_interval_(min_resubscribe_interval_ns),
        parser_(symbols, venue, std::move(dex)) {
    index_.fill(-1);
  }

  // `coin` is the venue's name of the instrument, exact case ("xyz:NVDA").
  bool add_instrument(InstrumentId id, std::string coin) {
    if (!symbols_.contains(id) || id.value >= kMaxInstruments || index_[id.value] >= 0)
      return false;
    index_[id.value] = static_cast<std::int16_t>(syncs_.size());
    syncs_.push_back(std::make_unique<HlBookSync>(id, venue_, sink_, requester_, min_interval_));
    coins_.push_back(std::move(coin));
    ids_.push_back(id);
    for (std::size_t i = 0; i < syncs_.size(); ++i) syncs_[i]->set_log_names(log_name_, coins_[i]);
    rebuild_payloads();
    return true;
  }
  void set_log_name(std::string_view venue) noexcept {
    log_name_ = venue;
    for (std::size_t i = 0; i < syncs_.size(); ++i) syncs_[i]->set_log_names(venue, coins_[i]);
  }
  [[nodiscard]] std::span<const InstrumentId> instruments() const noexcept { return ids_; }
  [[nodiscard]] const MdChannels& channels() const noexcept { return channels_; }

  // Unsubscribe and subscribe the coin's book again (a book that did not come in time).
  [[nodiscard]] std::vector<std::string> resubscribe_payloads(InstrumentId id) const {
    std::vector<std::string> out;
    if (id.value >= kMaxInstruments || index_[id.value] < 0 || !channels_.l2_book) return out;
    const std::string& coin = coins_[static_cast<std::size_t>(index_[id.value])];
    out.push_back(frame(false, "l2Book", coin));
    out.push_back(frame(true, "l2Book", coin));
    return out;
  }

  // ---- MarketDataFeed ----------------------------------------------------------------------
  ParseStatus on_message(std::string_view json, std::int64_t rx_ts) noexcept {
    ++stats_.messages;
    const Cycles t0 = rdtscp();
    const MdDecodeResult r = parser_.decode(json, wall_now(), t0, scratch_);
    switch (r.status) {
      case ParseStatus::Ok:
        break;
      case ParseStatus::Malformed:
        ++stats_.malformed;
        return r.status;
      case ParseStatus::UnknownSymbol:
        ++stats_.unknown_symbol;
        return r.status;
      case ParseStatus::Error:
        ++stats_.errors;
        last_error_.assign(r.error);
        return r.status;
      default:
        if (r.control == ControlOp::Pong) ++stats_.pongs;
        ++stats_.ignored;
        return r.status;
    }
    std::uint32_t off = 0;
    const auto t1 = static_cast<std::uint32_t>(rdtscp() - t0);
    for (std::uint32_t i = 0; i < r.count; ++i) {
      auto* h = reinterpret_cast<EventHeader*>(scratch_ + off);
      off += h->len;
      h->t1_delta = t1;
      if (h->type == EventType::BookSnapshot) {
        HlBookSync* s = sync(h->instrument);
        if (s == nullptr) return ParseStatus::UnknownSymbol;
        s->on_book(*reinterpret_cast<const BookDeltaMsg*>(h), rx_ts);
        ++stats_.pushed;
        continue;
      }
      if (!sink_.push(*h)) {
        ++stats_.dropped;
        continue;
      }
      ++stats_.pushed;
    }
    return ParseStatus::Ok;
  }
  void on_connected() noexcept {
    const std::int64_t now = steady_now().ns;
    if (channels_.l2_book) {
      for (auto& s : syncs_) s->start(now);
    }
  }
  void on_disconnected() noexcept {
    for (auto& s : syncs_) s->stop();
  }
  [[nodiscard]] std::span<const std::string> subscription_payloads() const noexcept {
    return payloads_;
  }
  void on_timer(std::int64_t now_ns) noexcept {
    for (auto& s : syncs_) s->on_timer(now_ns);
  }

  [[nodiscard]] HlBookSync* sync(InstrumentId id) noexcept {
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
  // The last text the venue sent on its "error" channel.
  [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

 private:
  static std::string frame(bool subscribe, std::string_view type, std::string_view coin) {
    char buf[256];
    const std::size_t n = write_subscription(subscribe, type, "coin", coin, buf);
    return std::string(buf, n);
  }
  void rebuild_payloads() {
    payloads_.clear();
    for (const std::string& coin : coins_) {
      if (channels_.l2_book) payloads_.push_back(frame(true, "l2Book", coin));
      if (channels_.bbo) payloads_.push_back(frame(true, "bbo", coin));
      if (channels_.trades) payloads_.push_back(frame(true, "trades", coin));
    }
  }

  const SymbolTable& symbols_;
  VenueId venue_;
  std::string_view log_name_ = "hyperliquid";
  EventSink& sink_;
  InstrumentCallback requester_;
  MdChannels channels_;
  std::int64_t min_interval_;
  HlMdParser parser_;
  std::array<std::int16_t, kMaxInstruments> index_{};
  std::vector<std::unique_ptr<HlBookSync>> syncs_;
  std::vector<std::string> coins_;
  std::vector<InstrumentId> ids_;
  std::vector<std::string> payloads_;
  std::string last_error_;
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
  MdFeedStats stats_;
};

static_assert(MarketDataFeed<HlMdFeed>);

}  // namespace fastmm::venues::hyperliquid
