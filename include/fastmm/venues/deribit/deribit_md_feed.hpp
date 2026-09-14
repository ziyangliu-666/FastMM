#pragma once
// DeribitMdFeed: the MarketDataFeed (feed.hpp) for the public Deribit connection. Owns the
// decoder, one DeribitBookSync per subscribed instrument and the market-data EventSink; runs on
// the venue's reactor thread and allocates nothing after add_instrument().
//
// Subscriptions (https://docs.deribit.com/api-reference/subscription-management/public-subscribe):
//   {"jsonrpc":"2.0","id":N,"method":"public/subscribe","params":{"channels":[...]}}
// with book.NAME.I, ticker.NAME.I and trades.NAME.I per instrument (I = 100ms by default; `raw`
// is for authorised connections only, which this one is not). The response `result` lists the
// channels actually subscribed; a shorter list means some were refused (docs, error 11052 note).
// public/subscribe costs 3,000 credits from a 30,000 pool (~3.3 requests/s, burst 10: rate-limits
// article), so channels are batched kInstrumentsPerSubscribe instruments per request.
// A book gap re-subscribes only that book channel: public/unsubscribe then public/subscribe, after
// which the venue sends a new snapshot.
#include "fastmm/core/time.hpp"
#include "fastmm/venues/deribit/deribit_book_sync.hpp"
#include "fastmm/venues/deribit/deribit_md_parser.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/symbology.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace fastmm::venues::deribit {

inline constexpr std::size_t kInstrumentsPerSubscribe = 32;
// JSON-RPC ids on the market-data connection.
inline constexpr std::int64_t kMdSubscribeIdBase = 1000;  // + batch index
inline constexpr std::int64_t kMdResubUnsubscribeId = 20;
inline constexpr std::int64_t kMdResubSubscribeId = 21;

struct MdFeedStats {
  std::uint64_t messages = 0;
  std::uint64_t pushed = 0;
  std::uint64_t dropped = 0;
  std::uint64_t malformed = 0;
  std::uint64_t ignored = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t subscribe_errors = 0;  // fewer channels subscribed than requested, or an error
  std::uint64_t test_requests = 0;
  std::uint64_t rpc_errors = 0;
};

struct MdIntervals {
  std::string book = "100ms";
  std::string ticker = "100ms";
  std::string trades = "100ms";
};

class DeribitMdFeed {
 public:
  DeribitMdFeed(const SymbolTable& symbols,
                const InstrumentTable& instruments,
                VenueId venue,
                EventSink& sink,
                ResubscribeRequester requester,
                MdIntervals intervals = {},
                std::int64_t min_resubscribe_interval_ns = DeribitBookSync::kDefaultMinInterval)
      : symbols_(symbols),
        venue_(venue),
        sink_(sink),
        requester_(requester),
        intervals_(std::move(intervals)),
        min_interval_(min_resubscribe_interval_ns),
        parser_(symbols, instruments, venue) {
    index_.fill(-1);
  }

  bool add_instrument(InstrumentId id) {
    if (!symbols_.contains(id) || id.value >= kMaxInstruments || index_[id.value] >= 0)
      return false;
    index_[id.value] = static_cast<std::int16_t>(syncs_.size());
    syncs_.push_back(
        std::make_unique<DeribitBookSync>(id, venue_, sink_, requester_, min_interval_));
    ids_.push_back(id);
    rebuild_payloads();
    return true;
  }
  [[nodiscard]] std::span<const InstrumentId> instruments() const noexcept { return ids_; }

  [[nodiscard]] std::string book_channel(InstrumentId id) const {
    return "book." + std::string(symbols_.venue_symbol(id)) + "." + intervals_.book;
  }
  [[nodiscard]] std::vector<std::string> channels(InstrumentId id) const {
    const std::string sym(symbols_.venue_symbol(id));
    return {book_channel(id),
            "ticker." + sym + "." + intervals_.ticker,
            "trades." + sym + "." + intervals_.trades};
  }
  // Control path (rare): unsubscribe + subscribe the book channel to obtain a new snapshot.
  [[nodiscard]] std::vector<std::string> resubscribe_payloads(InstrumentId id) const {
    const std::string ch = book_channel(id);
    return {R"({"jsonrpc":"2.0","id":)" + std::to_string(kMdResubUnsubscribeId) +
                R"(,"method":"public/unsubscribe","params":{"channels":[")" + ch + R"("]}})",
            R"({"jsonrpc":"2.0","id":)" + std::to_string(kMdResubSubscribeId) +
                R"(,"method":"public/subscribe","params":{"channels":[")" + ch + R"("]}})"};
  }

  // ---- MarketDataFeed ----------------------------------------------------------------------
  ParseStatus on_message(std::string_view json, std::int64_t rx_ts) noexcept {
    ++stats_.messages;
    const Cycles t0 = rdtscp();
    last_ = parser_.decode(json, wall_now(), t0, scratch_);
    switch (last_.status) {
      case ParseStatus::Ok:
        break;
      case ParseStatus::Malformed:
        ++stats_.malformed;
        return last_.status;
      case ParseStatus::UnknownSymbol:
        ++stats_.unknown_symbol;
        return last_.status;
      case ParseStatus::Error:
        ++stats_.rpc_errors;
        if (is_subscribe_id(last_.rpc.id)) ++stats_.subscribe_errors;
        return last_.status;
      default:
        if (last_.frame == FrameKind::TestRequest) {
          ++stats_.test_requests;
        } else if (last_.frame == FrameKind::Response && is_subscribe_id(last_.rpc.id)) {
          const auto batch = static_cast<std::size_t>(last_.rpc.id - kMdSubscribeIdBase);
          if (batch < expected_.size() && last_.result_items < expected_[batch])
            ++stats_.subscribe_errors;
        }
        ++stats_.ignored;
        return last_.status;
    }
    std::uint32_t off = 0;
    const auto t1 = static_cast<std::uint32_t>(rdtscp() - t0);
    for (std::uint32_t i = 0; i < last_.count; ++i) {
      auto* h = reinterpret_cast<EventHeader*>(scratch_ + off);
      off += h->len;
      h->t1_delta = t1;
      if (h->type == EventType::BookDelta || h->type == EventType::BookSnapshot) {
        DeribitBookSync* s = sync(h->instrument);
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
    for (auto& s : syncs_) s->start(now);
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

  // Classification of the frame last passed to on_message (control decisions in the venue).
  [[nodiscard]] const MdDecodeResult& last() const noexcept { return last_; }

  [[nodiscard]] DeribitBookSync* sync(InstrumentId id) noexcept {
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
  [[nodiscard]] bool is_subscribe_id(std::int64_t id) const noexcept {
    return id >= kMdSubscribeIdBase &&
           id < kMdSubscribeIdBase + static_cast<std::int64_t>(expected_.size());
  }
  void rebuild_payloads() {
    payloads_.clear();
    expected_.clear();
    for (std::size_t i = 0; i < ids_.size(); i += kInstrumentsPerSubscribe) {
      const std::size_t batch = i / kInstrumentsPerSubscribe;
      std::string p = R"({"jsonrpc":"2.0","id":)" +
                      std::to_string(kMdSubscribeIdBase + static_cast<std::int64_t>(batch)) +
                      R"(,"method":"public/subscribe","params":{"channels":[)";
      std::uint32_t n = 0;
      for (std::size_t k = i; k < ids_.size() && k < i + kInstrumentsPerSubscribe; ++k) {
        for (const std::string& ch : channels(ids_[k])) {
          if (n != 0) p += ',';
          p += '"';
          p += ch;
          p += '"';
          ++n;
        }
      }
      p += "]}}";
      payloads_.push_back(std::move(p));
      expected_.push_back(n);
    }
  }

  alignas(64) std::byte scratch_[kDecoderScratchBytes];  // first: keeps the padding small
  const SymbolTable& symbols_;
  VenueId venue_;
  EventSink& sink_;
  ResubscribeRequester requester_;
  MdIntervals intervals_;
  std::int64_t min_interval_;
  DeribitMdParser parser_;
  std::array<std::int16_t, kMaxInstruments> index_{};
  std::vector<std::unique_ptr<DeribitBookSync>> syncs_;
  std::vector<InstrumentId> ids_;
  std::vector<std::string> payloads_;
  std::vector<std::uint32_t> expected_;  // channels requested per subscribe batch
  MdDecodeResult last_{};
  MdFeedStats stats_;
};

static_assert(MarketDataFeed<DeribitMdFeed>);

}  // namespace fastmm::venues::deribit
