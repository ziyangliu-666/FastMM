#pragma once
// OkxMdFeed: the MarketDataFeed (feed.hpp) for OKX v5. Owns the decoder, one OkxBookSync (and one
// OkxShadowBook) per subscribed instrument and the market-data EventSink; runs on the venue's
// reactor thread and allocates nothing after add_instrument().
//
// Subscriptions (https://www.okx.com/docs-v5/en/#overview-websocket-subscribe, read 2026-09-26):
// {"id":..,"op":"subscribe","args":[{"channel":C,"instId":I},..]}, args at most 64 KB a request,
// subscribe + unsubscribe + login at most 480 an hour per connection. Channels per instrument:
// the depth channel (`books` by default, 400 levels every 100 ms; `books50-l2-tbt` and
// `books-l2-tbt` every 10 ms need VIP4 and a login), `bbo-tbt` (top of book every 10 ms) and
// `trades` (aggregated per taker order and price; `side` is the taker's).
//
// Book integrity is the seqId chain (okx_book_sync.hpp). The checksum was deprecated on
// 2026-06-23 and has been 0 since; when a snapshot carries a non-zero one the feed keeps the book's
// text and checks every push against it until the next snapshot, and when it is 0 the book costs
// nothing beyond the chain.
#include "fastmm/core/time.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/okx/okx_book_sync.hpp"
#include "fastmm/venues/okx/okx_md_parser.hpp"
#include "fastmm/venues/symbology.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fastmm::venues::okx {

struct MdFeedStats {
  std::uint64_t messages = 0;
  std::uint64_t pushed = 0;
  std::uint64_t dropped = 0;
  std::uint64_t malformed = 0;
  std::uint64_t ignored = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t subscribe_errors = 0;
  std::uint64_t pongs = 0;
  std::uint64_t checksums_checked = 0;
  std::uint64_t checksum_errors = 0;
};

class OkxMdFeed {
 public:
  OkxMdFeed(const SymbolTable& symbols,
            VenueId venue,
            EventSink& sink,
            ResubscribeRequester requester,
            OkxDepthChannel depth = OkxDepthChannel::Books,
            std::int64_t min_resubscribe_interval_ns = OkxBookSync::kDefaultMinInterval)
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
    ids_.push_back(id);
    rebuild_payloads();
    return true;
  }
  [[nodiscard]] std::span<const InstrumentId> instruments() const noexcept { return ids_; }
  [[nodiscard]] OkxDepthChannel depth() const noexcept { return depth_; }

  // Control path (rare): unsubscribe + subscribe the depth channel for a new snapshot.
  [[nodiscard]] std::vector<std::string> resubscribe_payloads(InstrumentId id) const {
    const std::string arg = R"({"channel":")" + std::string(to_string(depth_)) + R"(","instId":")" +
                            std::string(symbols_.venue_symbol(id)) + R"("})";
    return {R"({"id":"unsub","op":"unsubscribe","args":[)" + arg + "]}",
            R"({"id":"resub","op":"subscribe","args":[)" + arg + "]}"};
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
        return r.status;
      case ParseStatus::UnknownSymbol:
        ++stats_.unknown_symbol;
        return r.status;
      case ParseStatus::Error:
        if (r.control == ControlOp::Error) ++stats_.subscribe_errors;  // venue logs it
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
      if (h->type == EventType::BookDelta || h->type == EventType::BookSnapshot) {
        Book* b = book(h->instrument);
        if (b == nullptr) return ParseStatus::UnknownSymbol;
        on_book(*b, *reinterpret_cast<const BookDeltaMsg*>(h), r, rx_ts);
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
    for (auto& b : books_) {
      b->checked = false;
      b->shadow.clear();
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

  [[nodiscard]] OkxBookSync* sync(InstrumentId id) noexcept {
    Book* b = book(id);
    return b != nullptr ? &b->sync : nullptr;
  }
  [[nodiscard]] const OkxShadowBook* shadow(InstrumentId id) noexcept {
    Book* b = book(id);
    return b != nullptr ? &b->shadow : nullptr;
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
  // The decode result of the last frame (control events: code, message, channel).
  [[nodiscard]] const MdDecodeResult& last() const noexcept { return last_; }

 private:
  struct Book {
    Book(InstrumentId id,
         VenueId venue,
         EventSink& sink,
         ResubscribeRequester requester,
         std::int64_t min_interval)
        : sync(id, venue, sink, requester, min_interval) {}
    OkxBookSync sync;
    OkxShadowBook shadow;
    bool checked = false;  // the last snapshot carried a checksum: keep and check the text
  };

  [[nodiscard]] Book* book(InstrumentId id) noexcept {
    if (id.value >= kMaxInstruments || index_[id.value] < 0) return nullptr;
    return books_[static_cast<std::size_t>(index_[id.value])].get();
  }

  void on_book(Book& b, const BookDeltaMsg& d, const MdDecodeResult& r, std::int64_t rx) noexcept {
    const std::uint64_t before = b.sync.last_update_id();
    const bool was_synced = b.sync.synced();
    b.sync.on_book(d, rx);
    if (!b.sync.synced()) return;
    const bool snapshot = d.is_snapshot();
    if (snapshot) {
      b.checked = r.has_checksum;
      b.shadow.clear();
    } else if (!was_synced || d.prev_update_id != before) {
      return;  // not applied
    }
    if (!b.checked) return;
    const std::span<const OkxLevelText> texts = parser_.book_texts();
    const std::uint32_t nb = parser_.book_bid_count();
    bool ok = true;
    for (std::uint32_t i = 0; i < texts.size(); ++i) ok = b.shadow.apply(i < nb, texts[i]) && ok;
    ++stats_.checksums_checked;
    // An update without a checksum (0) after a snapshot with one: nothing to compare.
    if (ok && (!r.has_checksum || b.shadow.checksum() == r.checksum)) return;
    ++stats_.checksum_errors;
    b.checked = false;
    b.sync.resync(SyncReason::ChecksumMismatch, rx);
  }

  void rebuild_payloads() {
    payloads_.clear();
    // One request for all: three args per instrument are far below the 64 KB bound.
    std::string p = R"({"id":"md","op":"subscribe","args":[)";
    bool first = true;
    for (InstrumentId id : ids_) {
      const std::string sym(symbols_.venue_symbol(id));
      for (std::string_view ch :
           {to_string(depth_), std::string_view("bbo-tbt"), std::string_view("trades")}) {
        if (!first) p += ',';
        first = false;
        p += R"({"channel":")";
        p += ch;
        p += R"(","instId":")";
        p += sym;
        p += R"("})";
      }
    }
    p += "]}";
    payloads_.push_back(std::move(p));
  }

  const SymbolTable& symbols_;
  VenueId venue_;
  EventSink& sink_;
  ResubscribeRequester requester_;
  OkxDepthChannel depth_;
  std::int64_t min_interval_;
  OkxMdParser parser_;
  std::array<std::int16_t, kMaxInstruments> index_{};
  std::vector<std::unique_ptr<Book>> books_;
  std::vector<InstrumentId> ids_;
  std::vector<std::string> payloads_;
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
  MdFeedStats stats_;
  MdDecodeResult last_{};
};

static_assert(MarketDataFeed<OkxMdFeed>);

}  // namespace fastmm::venues::okx
