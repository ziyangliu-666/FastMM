#pragma once
// CoinbaseMdFeed: the MarketDataFeed (feed.hpp) for the Coinbase Exchange public feed. Owns the
// decoder, one book sync per subscribed product and the market-data EventSink; runs on the
// venue's reactor thread and allocates nothing after add_instrument().
//
// Subscription (https://docs.cdp.coinbase.com/exchange/websocket-feed/channels, read 2026-09-30):
//   {"type":"subscribe","product_ids":[..],"channels":[<depth>,"matches","heartbeat"]}
// <depth> is `level2_batch` (no authentication, batched every 50 ms; the venue acknowledges it as
// "level2_50") or `level2` (authenticated: the connector adds the signature). Both send the whole
// book as a snapshot, then updates.
//
// Book sync. The level2 channels carry no sequence number, so a lost update cannot be seen in the
// book stream itself. The feed numbers the book messages of each product itself (snapshot n,
// update k: prev k - 1, last k; CoinbaseSyncTraits) and watches for loss on the same connection
// with the trade ids, which the venue gives each product in steps of one:
//   * a `match` whose trade_id is not the previous one plus one (a trade message was lost);
//   * a heartbeat's last_trade_id (sent every second) above every trade_id seen by the next
//     heartbeat (the trade it names never came).
// Either resyncs the product's book (SyncReason::SequenceGap): Resyncing, then an unsubscribe and
// subscribe of the depth channel for a new snapshot, at most every 2 s (StreamBookSync). The
// venue's documentation says the level2 channel "guarantees delivery of all updates"; the check
// is for a connection that loses messages without closing.
#include "fastmm/core/time.hpp"
#include "fastmm/venues/coinbase/coinbase_book_sync.hpp"
#include "fastmm/venues/coinbase/coinbase_md_parser.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/symbology.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fastmm::venues::coinbase {

enum class DepthChannel : std::uint8_t { Level2Batch = 0, Level2 = 1 };
[[nodiscard]] constexpr std::string_view to_string(DepthChannel c) noexcept {
  return c == DepthChannel::Level2 ? "level2" : "level2_batch";
}

struct MdFeedStats {
  std::uint64_t messages = 0;
  std::uint64_t pushed = 0;
  std::uint64_t dropped = 0;
  std::uint64_t malformed = 0;
  std::uint64_t ignored = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t errors = 0;
  std::uint64_t trade_gaps = 0;      // a match's trade_id skipped one
  std::uint64_t heartbeat_gaps = 0;  // a heartbeat named a trade that never came
};

class CoinbaseMdFeed {
 public:
  CoinbaseMdFeed(const SymbolTable& symbols,
                 VenueId venue,
                 EventSink& sink,
                 ResubscribeRequester requester,
                 DepthChannel depth = DepthChannel::Level2Batch,
                 std::int64_t min_resubscribe_interval_ns = CoinbaseBookSync::kDefaultMinInterval)
      : symbols_(symbols),
        venue_(venue),
        sink_(sink),
        requester_(requester),
        depth_(depth),
        min_interval_(min_resubscribe_interval_ns),
        parser_(symbols, venue) {
    index_.fill(-1);
  }

  bool add_instrument(InstrumentId id) {
    if (!symbols_.contains(id) || id.value >= kMaxInstruments || index_[id.value] >= 0)
      return false;
    index_[id.value] = static_cast<std::int16_t>(books_.size());
    books_.push_back(std::make_unique<Book>(id, venue_, sink_, requester_, min_interval_));
    books_.back()->sync.set_log_names(log_name_, symbols_.venue_symbol(id));
    ids_.push_back(id);
    rebuild_payloads();
    return true;
  }
  // The connector's name for the syncs' log lines; it must outlive the feed.
  void set_log_name(std::string_view venue) noexcept {
    log_name_ = venue;
    for (std::size_t i = 0; i < books_.size(); ++i)
      books_[i]->sync.set_log_names(venue, symbols_.venue_symbol(ids_[i]));
  }
  [[nodiscard]] std::span<const InstrumentId> instruments() const noexcept { return ids_; }
  [[nodiscard]] DepthChannel depth() const noexcept { return depth_; }

  // Control path (rare): unsubscribe + subscribe the depth channel of one product for a new
  // snapshot. Unsigned: a level2 subscription gets its signature from the connector.
  [[nodiscard]] std::vector<std::string> resubscribe_payloads(InstrumentId id) const {
    const std::string args = R"(,"product_ids":[")" + std::string(symbols_.venue_symbol(id)) +
                             R"("],"channels":[")" + std::string(to_string(depth_)) + R"("]})";
    return {R"({"type":"unsubscribe")" + args, R"({"type":"subscribe")" + args};
  }

  // ---- MarketDataFeed ----------------------------------------------------------------------
  ParseStatus on_message(std::string_view json, std::int64_t rx_ts) noexcept {
    ++stats_.messages;
    const Cycles t0 = rdtscp();
    const MdDecodeResult r = parser_.decode(json, wall_now(), t0, scratch_);
    last_ = r;
    switch (r.status) {
      case ParseStatus::Ok:
        break;
      case ParseStatus::Malformed:
        ++stats_.malformed;
        // A snapshot out of order or unreadable leaves the book behind: start it over.
        if (r.instrument.valid()) resync_instrument(r.instrument, SyncReason::Explicit, rx_ts);
        return r.status;
      case ParseStatus::UnknownSymbol:
        ++stats_.unknown_symbol;
        return r.status;
      case ParseStatus::Error:
        ++stats_.errors;
        return r.status;
      case ParseStatus::Overflow:  // a book side too large to decode: lost like a full ring
        ++stats_.dropped;
        if (r.instrument.valid())
          resync_instrument(r.instrument, SyncReason::BufferOverflow, rx_ts);
        return r.status;
      default:
        ++stats_.ignored;
        if (r.control == ControlOp::Heartbeat) on_heartbeat(r, rx_ts);
        if (r.control == ControlOp::LastMatch) on_last_match(r);
        return r.status;
    }
    auto* h = reinterpret_cast<EventHeader*>(scratch_);
    h->t1_delta = static_cast<std::uint32_t>(rdtscp() - t0);
    Book* b = book(h->instrument);
    if (b == nullptr) return ParseStatus::UnknownSymbol;
    if (h->type == EventType::Trade) {
      on_trade(*b, r, rx_ts);
      if (!sink_.push(*h)) {
        ++stats_.dropped;
        return ParseStatus::Ok;
      }
      ++stats_.pushed;
      return ParseStatus::Ok;
    }
    auto* d = reinterpret_cast<BookDeltaMsg*>(scratch_);
    number_book_message(*d, b->counter);
    b->sync.on_book(*d, rx_ts);
    ++stats_.pushed;
    return ParseStatus::Ok;
  }
  void on_connected() noexcept {
    const std::int64_t now = steady_now().ns;
    for (auto& b : books_) {
      b->last_trade_id = 0;
      b->heartbeat_trade_id = 0;
      b->sync.start(now);
    }
  }
  void on_disconnected() noexcept {
    for (auto& b : books_) b->sync.stop();
  }
  [[nodiscard]] std::span<const std::string> subscription_payloads() const noexcept {
    return payloads_;
  }

  void on_timer(std::int64_t now_ns) noexcept {
    for (auto& b : books_) b->sync.on_timer(now_ns);
  }

  [[nodiscard]] CoinbaseBookSync* sync(InstrumentId id) noexcept {
    Book* b = book(id);
    return b != nullptr ? &b->sync : nullptr;
  }
  [[nodiscard]] std::uint32_t synced_count() const noexcept {
    std::uint32_t n = 0;
    for (const auto& b : books_) n += b->sync.synced() ? 1U : 0U;
    return n;
  }
  [[nodiscard]] std::uint64_t resync_count() const noexcept {
    std::uint64_t n = 0;
    for (const auto& b : books_) n += b->sync.resync_count();
    return n;
  }
  [[nodiscard]] const MdFeedStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const MdParserStats& parser_stats() const noexcept { return parser_.stats(); }
  // The decode result of the last frame (control events: message, reason).
  [[nodiscard]] const MdDecodeResult& last() const noexcept { return last_; }

 private:
  struct Book {
    Book(InstrumentId id,
         VenueId venue,
         EventSink& sink,
         ResubscribeRequester requester,
         std::int64_t min_interval)
        : sync(id, venue, sink, requester, min_interval) {}
    CoinbaseBookSync sync;
    std::uint64_t counter = 0;             // the feed's update ids
    std::uint64_t last_trade_id = 0;       // highest trade_id seen (match, last_match)
    std::uint64_t heartbeat_trade_id = 0;  // last_trade_id of the previous heartbeat
  };

  [[nodiscard]] Book* book(InstrumentId id) noexcept {
    if (id.value >= kMaxInstruments || index_[id.value] < 0) return nullptr;
    return books_[static_cast<std::size_t>(index_[id.value])].get();
  }

  void resync_instrument(InstrumentId id, SyncReason reason, std::int64_t now) noexcept {
    if (Book* b = book(id)) b->sync.resync(reason, now);
  }

  void on_trade(Book& b, const MdDecodeResult& r, std::int64_t now) noexcept {
    const std::uint64_t id = r.trade_id;
    if (b.last_trade_id != 0 && id > b.last_trade_id + 1) {
      ++stats_.trade_gaps;
      FASTMM_LOG_WARN("{}: {} trade ids skipped from {} to {}: the connection lost messages",
                      log_name_,
                      symbols_.venue_symbol(b.sync.instrument()),
                      b.last_trade_id,
                      id);
      b.sync.resync(SyncReason::SequenceGap, now);
    }
    if (id > b.last_trade_id) b.last_trade_id = id;
  }
  void on_last_match(const MdDecodeResult& r) noexcept {
    Book* b = book(r.instrument);
    if (b != nullptr && r.trade_id > b->last_trade_id) b->last_trade_id = r.trade_id;
  }
  // The previous heartbeat named a trade that has still not come a heartbeat later.
  void on_heartbeat(const MdDecodeResult& r, std::int64_t now) noexcept {
    Book* b = book(r.instrument);
    if (b == nullptr) return;
    if (b->heartbeat_trade_id != 0 && b->last_trade_id != 0 &&
        b->heartbeat_trade_id > b->last_trade_id) {
      ++stats_.heartbeat_gaps;
      FASTMM_LOG_WARN("{}: {} heartbeat named trade {}, the last one received is {}",
                      log_name_,
                      symbols_.venue_symbol(b->sync.instrument()),
                      b->heartbeat_trade_id,
                      b->last_trade_id);
      b->last_trade_id = b->heartbeat_trade_id;  // reported once
      b->sync.resync(SyncReason::SequenceGap, now);
    }
    b->heartbeat_trade_id = r.trade_id;
  }

  void rebuild_payloads() {
    payloads_.clear();
    std::string p = R"({"type":"subscribe","product_ids":[)";
    bool first = true;
    for (InstrumentId id : ids_) {
      if (!first) p += ',';
      first = false;
      p += '"';
      p += symbols_.venue_symbol(id);
      p += '"';
    }
    p += R"(],"channels":[")";
    p += to_string(depth_);
    p += R"(","matches","heartbeat"]})";
    payloads_.push_back(std::move(p));
  }

  const SymbolTable& symbols_;
  VenueId venue_;
  std::string_view log_name_ = "coinbase";
  EventSink& sink_;
  ResubscribeRequester requester_;
  DepthChannel depth_;
  std::int64_t min_interval_;
  CoinbaseMdParser parser_;
  std::array<std::int16_t, kMaxInstruments> index_{};
  std::vector<std::unique_ptr<Book>> books_;
  std::vector<InstrumentId> ids_;
  std::vector<std::string> payloads_;
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
  MdFeedStats stats_;
  MdDecodeResult last_{};
};

static_assert(MarketDataFeed<CoinbaseMdFeed>);

}  // namespace fastmm::venues::coinbase
