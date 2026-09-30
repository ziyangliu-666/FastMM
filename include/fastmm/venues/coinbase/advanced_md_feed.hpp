#pragma once
// AdvancedMdFeed: the MarketDataFeed (feed.hpp) for the Coinbase Advanced Trade public feed. Owns
// the decoder, one book sync per subscribed product and the market-data EventSink; runs on the
// venue's reactor thread and allocates nothing after add_instrument().
//
// Subscriptions (AsyncAPI, read 2026-09-30), one channel a message:
//   {"type":"subscribe","product_ids":[..],"channel":"level2"}   (and "market_trades")
//   {"type":"subscribe","channel":"heartbeats"}   keeps the others open ("Most channels close
//                                                 within 60-90 seconds if no updates are sent")
//
// Book sync. level2 "guarantees delivery of all updates", and every message of the connection
// carries sequence_num, which increases by one per message across every channel. The feed numbers
// each product's book messages itself (coinbase_book_sync.hpp), and a sequence_num that is not the
// previous one plus one resyncs every book of the connection (SyncReason::SequenceGap): Resyncing,
// then an unsubscribe and subscribe of level2 for each product for new snapshots, at most every
// 2 s per product (StreamBookSync).
#include "fastmm/core/time.hpp"
#include "fastmm/venues/coinbase/advanced_md_parser.hpp"
#include "fastmm/venues/coinbase/coinbase_book_sync.hpp"
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

struct AdvancedFeedStats {
  std::uint64_t messages = 0;
  std::uint64_t pushed = 0;
  std::uint64_t dropped = 0;
  std::uint64_t malformed = 0;
  std::uint64_t ignored = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t errors = 0;
  std::uint64_t sequence_gaps = 0;  // a sequence_num that skipped
};

class AdvancedMdFeed {
 public:
  AdvancedMdFeed(const SymbolTable& symbols,
                 VenueId venue,
                 EventSink& sink,
                 ResubscribeRequester requester,
                 std::int64_t min_resubscribe_interval_ns = CoinbaseBookSync::kDefaultMinInterval)
      : symbols_(symbols),
        venue_(venue),
        sink_(sink),
        requester_(requester),
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
  void set_log_name(std::string_view venue) noexcept {
    log_name_ = venue;
    for (std::size_t i = 0; i < books_.size(); ++i)
      books_[i]->sync.set_log_names(venue, symbols_.venue_symbol(ids_[i]));
  }
  [[nodiscard]] std::span<const InstrumentId> instruments() const noexcept { return ids_; }

  // Control path (rare): unsubscribe + subscribe level2 of one product for a new snapshot.
  [[nodiscard]] std::vector<std::string> resubscribe_payloads(InstrumentId id) const {
    const std::string args = R"(,"product_ids":[")" + std::string(symbols_.venue_symbol(id)) +
                             R"("],"channel":"level2"})";
    return {R"({"type":"unsubscribe")" + args, R"({"type":"subscribe")" + args};
  }

  // ---- MarketDataFeed ----------------------------------------------------------------------
  ParseStatus on_message(std::string_view json, std::int64_t rx_ts) noexcept {
    ++stats_.messages;
    const Cycles t0 = rdtscp();
    const AdvancedMdResult r = parser_.decode(json, wall_now(), t0, scratch_);
    last_ = r;
    // Every message of the connection counts, whatever it carries.
    if (r.has_sequence) check_sequence(r.sequence, rx_ts);
    switch (r.status) {
      case ParseStatus::Ok:
        break;
      case ParseStatus::Malformed:
        ++stats_.malformed;
        if (r.instrument.valid()) resync_instrument(r.instrument, SyncReason::Explicit, rx_ts);
        return r.status;
      case ParseStatus::UnknownSymbol:
        ++stats_.unknown_symbol;
        return r.status;
      case ParseStatus::Error:
        ++stats_.errors;
        return r.status;
      case ParseStatus::Overflow:
        ++stats_.dropped;
        if (r.instrument.valid())
          resync_instrument(r.instrument, SyncReason::BufferOverflow, rx_ts);
        return r.status;
      default:
        ++stats_.ignored;
        return r.status;
    }
    std::uint32_t off = 0;
    const auto t1 = static_cast<std::uint32_t>(rdtscp() - t0);
    for (std::uint32_t i = 0; i < r.count; ++i) {
      auto* h = reinterpret_cast<EventHeader*>(scratch_ + off);
      off += h->len;
      h->t1_delta = t1;
      Book* b = book(h->instrument);
      if (b == nullptr) continue;
      if (h->type == EventType::BookDelta || h->type == EventType::BookSnapshot) {
        auto* d = reinterpret_cast<BookDeltaMsg*>(h);
        number_book_message(*d, b->counter);
        b->sync.on_book(*d, rx_ts);
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
    have_sequence_ = false;
    for (auto& b : books_) b->sync.start(now);
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
  [[nodiscard]] const AdvancedFeedStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const AdvancedMdStats& parser_stats() const noexcept { return parser_.stats(); }
  [[nodiscard]] const AdvancedMdResult& last() const noexcept { return last_; }

 private:
  struct Book {
    Book(InstrumentId id,
         VenueId venue,
         EventSink& sink,
         ResubscribeRequester requester,
         std::int64_t min_interval)
        : sync(id, venue, sink, requester, min_interval) {}
    CoinbaseBookSync sync;
    std::uint64_t counter = 0;
  };

  [[nodiscard]] Book* book(InstrumentId id) noexcept {
    if (id.value >= kMaxInstruments || index_[id.value] < 0) return nullptr;
    return books_[static_cast<std::size_t>(index_[id.value])].get();
  }
  void resync_instrument(InstrumentId id, SyncReason reason, std::int64_t now) noexcept {
    if (Book* b = book(id)) b->sync.resync(reason, now);
  }
  void check_sequence(std::uint64_t seq, std::int64_t now) noexcept {
    if (have_sequence_ && seq != sequence_ + 1) {
      ++stats_.sequence_gaps;
      FASTMM_LOG_WARN("{}: sequence_num went from {} to {}: the connection lost messages",
                      log_name_,
                      sequence_,
                      seq);
      for (auto& b : books_) b->sync.resync(SyncReason::SequenceGap, now);
    }
    sequence_ = seq;
    have_sequence_ = true;
  }

  void rebuild_payloads() {
    payloads_.clear();
    std::string ids;
    for (InstrumentId id : ids_) {
      if (!ids.empty()) ids += ',';
      ids += '"';
      ids += symbols_.venue_symbol(id);
      ids += '"';
    }
    for (const char* ch : {"level2", "market_trades"})
      payloads_.push_back(R"({"type":"subscribe","product_ids":[)" + ids + R"(],"channel":")" + ch +
                          "\"}");
    payloads_.emplace_back(R"({"type":"subscribe","channel":"heartbeats"})");
  }

  const SymbolTable& symbols_;
  VenueId venue_;
  std::string_view log_name_ = "coinbase";
  EventSink& sink_;
  ResubscribeRequester requester_;
  std::int64_t min_interval_;
  AdvancedMdParser parser_;
  std::array<std::int16_t, kMaxInstruments> index_{};
  std::vector<std::unique_ptr<Book>> books_;
  std::vector<InstrumentId> ids_;
  std::vector<std::string> payloads_;
  bool have_sequence_ = false;
  std::uint64_t sequence_ = 0;
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
  AdvancedFeedStats stats_;
  AdvancedMdResult last_{};
};

static_assert(MarketDataFeed<AdvancedMdFeed>);

}  // namespace fastmm::venues::coinbase
