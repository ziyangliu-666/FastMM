#pragma once
// GeminiMdFeed: the MarketDataFeed (feed.hpp) for Gemini's WebSocket API. Owns the decoder, one
// GeminiBookSync per subscribed instrument and the market-data EventSink; runs on the venue's
// reactor thread and allocates nothing after add_instrument().
//
// Subscriptions (https://developer.gemini.com/websocket/streams.md, read 2026-09-30), one request
// {"id":"md","method":"subscribe","params":[..]} with three streams per symbol:
// `{symbol}@depth@100ms` (differential depth, the snapshot first: the connection URL carries
// `snapshot=-1`), `{symbol}@bookTicker` (real time) and `{symbol}@trade`.
//
// Which depth frame is the snapshot (gemini_book_sync.hpp): the first of a symbol after the
// connection opens, and after a resubscription the first after the unsubscribe's reply. Measured
// on production 2026-09-30: the replies to an unsubscribe and a subscribe sent together arrive
// before the new snapshot, while on a fresh connection the snapshot came before the subscribe's
// reply; the unsubscribe's reply precedes the snapshot either way.
#include "fastmm/core/time.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/gemini/gemini_book_sync.hpp"
#include "fastmm/venues/gemini/gemini_md_parser.hpp"
#include "fastmm/venues/symbology.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fastmm::venues::gemini {

struct MdFeedStats {
  std::uint64_t messages = 0;
  std::uint64_t pushed = 0;
  std::uint64_t dropped = 0;
  std::uint64_t malformed = 0;
  std::uint64_t ignored = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t request_errors = 0;
  std::uint64_t snapshots = 0;
  std::uint64_t superseded = 0;  // depth frames of a subscription being replaced
};

class GeminiMdFeed {
 public:
  GeminiMdFeed(const SymbolTable& symbols,
               VenueId venue,
               EventSink& sink,
               InstrumentCallback requester,
               std::int64_t min_resubscribe_interval_ns = GeminiBookSync::kDefaultMinInterval)
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
  // The connector's name for the syncs' log lines; it must outlive the feed.
  void set_log_name(std::string_view venue) noexcept {
    log_name_ = venue;
    for (std::size_t i = 0; i < books_.size(); ++i)
      books_[i]->sync.set_log_names(venue, symbols_.venue_symbol(ids_[i]));
  }
  [[nodiscard]] std::span<const InstrumentId> instruments() const noexcept { return ids_; }

  // Control path (rare): unsubscribe + subscribe the depth stream for a new snapshot. Frames of
  // the old subscription are dropped until the unsubscribe is answered.
  [[nodiscard]] std::vector<std::string> resubscribe_payloads(InstrumentId id) {
    Book* b = book(id);
    if (b == nullptr) return {};
    b->awaiting_unsubscribe = true;
    b->expect_snapshot = false;
    const std::string k = std::to_string(index_[id.value]);
    const std::string stream = "\"" + std::string(symbols_.lower_symbol(id)) + "@depth@100ms\"";
    return {R"({"id":"unsub-)" + k + R"(","method":"unsubscribe","params":[)" + stream + "]}",
            R"({"id":"resub-)" + k + R"(","method":"subscribe","params":[)" + stream + "]}"};
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
        ++stats_.request_errors;  // the venue logs it
        return r.status;
      case ParseStatus::Overflow:  // a side past LevelSpill::kCapacity: lost like a full ring
        ++stats_.dropped;
        return r.status;
      default:
        if (r.control.present) on_reply(r.control);
        ++stats_.ignored;
        return r.status;
    }
    auto* h = reinterpret_cast<EventHeader*>(scratch_);
    h->t1_delta = static_cast<std::uint32_t>(rdtscp() - t0);
    if (h->type == EventType::BookDelta) {
      Book* b = book(h->instrument);
      if (b == nullptr) return ParseStatus::UnknownSymbol;
      auto* d = reinterpret_cast<BookDeltaMsg*>(h);
      if (b->awaiting_unsubscribe) {
        ++stats_.superseded;
        return ParseStatus::Ignored;
      }
      if (b->expect_snapshot) {
        b->expect_snapshot = false;
        d->hdr.type = EventType::BookSnapshot;
        d->hdr.flags =
            static_cast<std::uint8_t>((d->hdr.flags | EventHeader::kSnapshot) &
                                      ~(EventHeader::kTruncatedBids | EventHeader::kTruncatedAsks));
        ++stats_.snapshots;
      }
      b->sync.on_book(*d, rx_ts);
      ++stats_.pushed;
      return ParseStatus::Ok;
    }
    if (!sink_.push(*h)) {
      ++stats_.dropped;
      return ParseStatus::Ok;
    }
    ++stats_.pushed;
    return ParseStatus::Ok;
  }
  void on_connected() noexcept {
    const std::int64_t now = steady_now().ns;
    for (auto& b : books_) {
      b->expect_snapshot = true;
      b->awaiting_unsubscribe = false;
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

  [[nodiscard]] GeminiBookSync* sync(InstrumentId id) noexcept {
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
  // The decode result of the last frame (replies: id, status, error, serverTime).
  [[nodiscard]] const MdDecodeResult& last() const noexcept { return last_; }

 private:
  struct Book {
    Book(InstrumentId id,
         VenueId venue,
         EventSink& sink,
         InstrumentCallback requester,
         std::int64_t min_interval)
        : sync(id, venue, sink, requester, min_interval) {}
    GeminiBookSync sync;
    bool expect_snapshot = false;       // the next depth frame is the snapshot
    bool awaiting_unsubscribe = false;  // resubscribing: drop frames until the reply
  };

  [[nodiscard]] Book* book(InstrumentId id) noexcept {
    if (id.value >= kMaxInstruments || index_[id.value] < 0) return nullptr;
    return books_[static_cast<std::size_t>(index_[id.value])].get();
  }

  // "unsub-<k>" answered: book k's next depth frame is the new subscription's snapshot.
  void on_reply(const MdControl& c) noexcept {
    constexpr std::string_view kUnsub = "unsub-";
    if (c.id.substr(0, kUnsub.size()) != kUnsub) return;
    std::size_t k = 0;
    for (const char ch : c.id.substr(kUnsub.size())) {
      if (ch < '0' || ch > '9') return;
      k = k * 10 + static_cast<std::size_t>(ch - '0');
    }
    if (k >= books_.size()) return;
    books_[k]->awaiting_unsubscribe = false;
    books_[k]->expect_snapshot = true;
  }

  void rebuild_payloads() {
    payloads_.clear();
    std::string p = R"({"id":"md","method":"subscribe","params":[)";
    bool first = true;
    for (InstrumentId id : ids_) {
      const std::string sym(symbols_.lower_symbol(id));
      for (std::string_view suffix : {"@depth@100ms", "@bookTicker", "@trade"}) {
        if (!first) p += ',';
        first = false;
        p += '"';
        p += sym;
        p += suffix;
        p += '"';
      }
    }
    p += "]}";
    payloads_.push_back(std::move(p));
  }

  const SymbolTable& symbols_;
  VenueId venue_;
  std::string_view log_name_ = "gemini";
  EventSink& sink_;
  InstrumentCallback requester_;
  std::int64_t min_interval_;
  GeminiMdParser parser_;
  std::array<std::int16_t, kMaxInstruments> index_{};
  std::vector<std::unique_ptr<Book>> books_;
  std::vector<InstrumentId> ids_;
  std::vector<std::string> payloads_;
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
  MdFeedStats stats_;
  MdDecodeResult last_{};
};

static_assert(MarketDataFeed<GeminiMdFeed>);

}  // namespace fastmm::venues::gemini
