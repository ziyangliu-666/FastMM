#pragma once
// Alpaca stock market data: the IEX stream (wss://stream.data.alpaca.markets/v2/iex; v2/test
// streams the symbol FAKEPACA at any hour), "Real-time Stock Pricing Data" and "Streaming Market
// Data" in Alpaca's docs, read 2026-10-27. Every frame is a JSON array; data points of several
// symbols may share one, control messages come alone:
//
//   {"T":"q","S":"AAPL","bx":"V","bp":239.4,"bs":2,"ax":"V","ap":239.5,"as":1,"c":["R"],
//    "t":"2026-10-27T13:30:00.003818259Z","z":"C"}
//        -> BookSnapshot (kSnapshot) of one level a side: IEX's top of book. A zero price is an
//           empty side. Sizes are copied as the feed sends them (the docs say round lots).
//   {"T":"t","S":"AAPL","i":52983525029461,"x":"V","p":239.45,"s":100,"c":["@"],"t":..,"z":"C"}
//        -> TradeMsg, aggressor unknown (kNoAggressor)
//   {"T":"success","msg":"connected"|"authenticated"}, {"T":"subscription",..},
//   {"T":"error","code":402,"msg":"auth failed"} -> control
// Prices and sizes are JSON numbers, parsed from their text (no double).
//
// AlpacaMdFeed: the MarketDataFeed. Every quote is a whole book, so a book never has a gap; it is
// synced from its first quote and stays synced until the connection goes.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/book_sync.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/symbology.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::alpaca {

enum class MdControl : std::uint8_t {
  None = 0,
  Connected,
  Authenticated,
  Subscription,
  Error,
  Other,
};

struct MdParserStats {
  std::uint64_t frames = 0;
  std::uint64_t quotes = 0;
  std::uint64_t trades = 0;
  std::uint64_t control = 0;
  std::uint64_t ignored = 0;  // corrections, cancel errors, bars, statuses
  std::uint64_t malformed = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t overflow = 0;
};

struct MdDecodeResult : DecodeResult {
  std::uint32_t count = 0;  // messages written back to back
  MdControl control = MdControl::None;
  int error_code = 0;
  std::string_view msg;  // view into the frame
};

class AlpacaMdParser {
 public:
  static constexpr std::size_t kDefaultCapacity = 1U << 20;

  AlpacaMdParser(const SymbolTable& symbols,
                 VenueId venue,
                 std::size_t capacity = kDefaultCapacity);
  ~AlpacaMdParser();
  AlpacaMdParser(const AlpacaMdParser&) = delete;
  AlpacaMdParser& operator=(const AlpacaMdParser&) = delete;

  // `json` needs kJsonPadding readable bytes behind it; `out` must hold kDecoderScratchBytes.
  MdDecodeResult decode(std::string_view json,
                        Timestamp recv_ts,
                        Cycles t0,
                        std::span<std::byte> out) noexcept;
  [[nodiscard]] const MdParserStats& stats() const noexcept { return stats_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  const SymbolTable& symbols_;
  VenueId venue_;
  MdParserStats stats_;
};

// Every quote is a snapshot: no deltas, no gaps.
struct AlpacaBookTraits {
  static constexpr bool kNeedsRestSnapshot = false;
  static constexpr bool kBuffersDeltas = false;
  static constexpr bool is_stale(const BookDeltaMsg&, std::uint64_t) noexcept { return false; }
  static constexpr bool first_applies(const BookDeltaMsg&, std::uint64_t) noexcept { return true; }
  static constexpr bool next_applies(const BookDeltaMsg&, std::uint64_t) noexcept { return true; }
  static constexpr bool is_snapshot_marker(const BookDeltaMsg&) noexcept { return false; }
};
using AlpacaBookSync = StreamBookSync<AlpacaBookTraits>;

struct MdFeedStats {
  std::uint64_t messages = 0;
  std::uint64_t pushed = 0;
  std::uint64_t dropped = 0;
};

class AlpacaMdFeed {
 public:
  // A quiet symbol has no quote to wait for (the stream sends none on subscribing, and none
  // outside IEX's hours): a book that has not come is never asked for again.
  static constexpr std::int64_t kNoSnapshotTimeout = std::numeric_limits<std::int64_t>::max() / 4;

  AlpacaMdFeed(const SymbolTable& symbols, VenueId venue, EventSink& sink, bool trades = true)
      : symbols_(symbols), venue_(venue), sink_(sink), trades_(trades), parser_(symbols, venue) {
    index_.fill(-1);
  }

  bool add_instrument(InstrumentId id) {
    if (!symbols_.contains(id) || id.value >= kMaxInstruments || index_[id.value] >= 0)
      return false;
    index_[id.value] = static_cast<std::int16_t>(syncs_.size());
    syncs_.push_back(std::make_unique<AlpacaBookSync>(id,
                                                      venue_,
                                                      sink_,
                                                      InstrumentCallback{},
                                                      AlpacaBookSync::kDefaultMinInterval,
                                                      kNoSnapshotTimeout));
    syncs_.back()->set_log_names(log_name_, symbols_.venue_symbol(id));
    rebuild_payloads();
    return true;
  }
  void set_log_name(std::string_view venue) noexcept { log_name_ = venue; }

  // ---- MarketDataFeed ----------------------------------------------------------------------
  ParseStatus on_message(std::string_view json, std::int64_t rx_ts) noexcept {
    ++stats_.messages;
    const Cycles t0 = rdtscp();
    last_ = parser_.decode(json, wall_now(), t0, scratch_);
    if (last_.status != ParseStatus::Ok) return last_.status;
    std::uint32_t off = 0;
    const auto t1 = static_cast<std::uint32_t>(rdtscp() - t0);
    for (std::uint32_t i = 0; i < last_.count; ++i) {
      auto* h = reinterpret_cast<EventHeader*>(scratch_ + off);
      off += h->len;
      h->t1_delta = t1;
      if (h->type == EventType::BookSnapshot) {
        if (AlpacaBookSync* s = sync(h->instrument)) {
          s->on_book(*reinterpret_cast<const BookDeltaMsg*>(h), rx_ts);
          ++stats_.pushed;
        }
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
  // One subscribe frame for every symbol (sent after the auth frame, which the venue writes).
  [[nodiscard]] std::span<const std::string> subscription_payloads() const noexcept {
    return payloads_;
  }
  void on_timer(std::int64_t now_ns) noexcept {
    for (auto& s : syncs_) s->on_timer(now_ns);
  }

  [[nodiscard]] AlpacaBookSync* sync(InstrumentId id) noexcept {
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
  [[nodiscard]] const MdDecodeResult& last() const noexcept { return last_; }
  [[nodiscard]] const MdFeedStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const MdParserStats& parser_stats() const noexcept { return parser_.stats(); }

 private:
  void rebuild_payloads() {
    std::string list;
    for (const auto& s : syncs_) {
      if (!list.empty()) list += ',';
      list += '"';
      list += symbols_.venue_symbol(s->instrument());
      list += '"';
    }
    std::string p = R"({"action":"subscribe","quotes":[)" + list + "]";
    if (trades_) p += R"(,"trades":[)" + list + "]";
    p += '}';
    payloads_.clear();
    payloads_.push_back(std::move(p));
  }

  const SymbolTable& symbols_;
  VenueId venue_;
  std::string_view log_name_ = "alpaca";
  EventSink& sink_;
  bool trades_;
  AlpacaMdParser parser_;
  std::array<std::int16_t, kMaxInstruments> index_{};
  std::vector<std::unique_ptr<AlpacaBookSync>> syncs_;
  std::vector<std::string> payloads_;
  MdDecodeResult last_{};
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
  MdFeedStats stats_;
};

static_assert(MarketDataFeed<AlpacaMdFeed>);

}  // namespace fastmm::venues::alpaca
