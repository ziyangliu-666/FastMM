#pragma once
// LighterMdFeed: the MarketDataFeed (feed.hpp) for Lighter. Owns the decoder, one book sync per
// instrument and the market-data EventSink; runs on the venue's reactor thread.
//
// Subscriptions, one per market and channel ({"type":"subscribe","channel":"order_book/110"}):
// order_book (the whole book on subscribing, then the changed levels every 50 ms), ticker (the
// touch) and trade, each switchable. An update applies when its begin_nonce is the nonce the book
// is at; anything else, a book that did not come within the snapshot timeout or a push that
// overflowed the ring re-subscribes the market, which sends the book again.
#include "fastmm/core/time.hpp"
#include "fastmm/venues/book_sync.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/lighter/lighter_md_parser.hpp"
#include "fastmm/venues/symbology.hpp"

#include <fmt/format.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fastmm::venues::lighter {

// The snapshot's nonce is the begin_nonce of the update after it, and so on.
struct LighterBookTraits {
  static constexpr bool kNeedsRestSnapshot = false;
  static constexpr bool kBuffersDeltas = false;
  static constexpr bool is_stale(const BookDeltaMsg& d, std::uint64_t last) noexcept {
    return d.last_update_id <= last;
  }
  static constexpr bool first_applies(const BookDeltaMsg& d, std::uint64_t snap) noexcept {
    return d.prev_update_id == snap;
  }
  static constexpr bool next_applies(const BookDeltaMsg& d, std::uint64_t prev) noexcept {
    return d.prev_update_id == prev;
  }
  static constexpr bool is_snapshot_marker(const BookDeltaMsg&) noexcept { return false; }
};
using LighterBookSync = StreamBookSync<LighterBookTraits>;

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
  bool order_book = true;
  bool ticker = true;
  bool trades = true;
};

// {"type":"subscribe"|"unsubscribe","channel":"<name>/<market>"} with an optional auth token.
[[nodiscard]] inline std::string subscription_frame(bool subscribe,
                                                    std::string_view name,
                                                    std::int64_t id,
                                                    std::string_view auth = {}) {
  if (auth.empty())
    return fmt::format(
        R"({{"type":"{}","channel":"{}/{}"}})", subscribe ? "subscribe" : "unsubscribe", name, id);
  return fmt::format(R"({{"type":"{}","channel":"{}/{}","auth":"{}"}})",
                     subscribe ? "subscribe" : "unsubscribe",
                     name,
                     id,
                     auth);
}

class LighterMdFeed {
 public:
  LighterMdFeed(const SymbolTable& symbols,
                VenueId venue,
                EventSink& sink,
                InstrumentCallback requester,
                MdChannels channels = {},
                std::int64_t min_resubscribe_interval_ns = LighterBookSync::kDefaultMinInterval)
      : symbols_(symbols),
        venue_(venue),
        sink_(sink),
        requester_(requester),
        channels_(channels),
        min_interval_(min_resubscribe_interval_ns),
        parser_(markets_, venue) {
    index_.fill(-1);
  }

  // `market` is the venue's market index, `symbol` its name for the log.
  bool add_instrument(InstrumentId id, std::uint32_t market, std::string symbol) {
    if (!symbols_.contains(id) || id.value >= kMaxInstruments || index_[id.value] >= 0)
      return false;
    index_[id.value] = static_cast<std::int16_t>(syncs_.size());
    syncs_.push_back(
        std::make_unique<LighterBookSync>(id, venue_, sink_, requester_, min_interval_));
    names_.push_back(std::move(symbol));
    market_ids_.push_back(market);
    ids_.push_back(id);
    markets_.add(market, id);
    for (std::size_t i = 0; i < syncs_.size(); ++i) syncs_[i]->set_log_names(log_name_, names_[i]);
    rebuild_payloads();
    return true;
  }
  void set_log_name(std::string_view venue) noexcept {
    log_name_ = venue;
    for (std::size_t i = 0; i < syncs_.size(); ++i) syncs_[i]->set_log_names(venue, names_[i]);
  }
  [[nodiscard]] std::span<const InstrumentId> instruments() const noexcept { return ids_; }
  [[nodiscard]] const MdChannels& channels() const noexcept { return channels_; }
  [[nodiscard]] const MarketMap& markets() const noexcept { return markets_; }

  // Unsubscribe and subscribe the market's book again: the venue sends it whole.
  [[nodiscard]] std::vector<std::string> resubscribe_payloads(InstrumentId id) const {
    std::vector<std::string> out;
    if (id.value >= kMaxInstruments || index_[id.value] < 0 || !channels_.order_book) return out;
    const std::uint32_t m = market_ids_[static_cast<std::size_t>(index_[id.value])];
    out.push_back(subscription_frame(false, "order_book", m));
    out.push_back(subscription_frame(true, "order_book", m));
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
      case ParseStatus::Overflow: {
        // A book bigger than a message holds: resubscribing will not shrink it, but the sync
        // marks the book unusable rather than applying a part.
        const InstrumentId id = markets_.find(market_of_channel(r.channel));
        if (LighterBookSync* s = sync(id)) s->resync(SyncReason::BufferOverflow, rx_ts);
        ++stats_.dropped;
        return r.status;
      }
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
      if (h->type == EventType::BookSnapshot || h->type == EventType::BookDelta) {
        LighterBookSync* s = sync(h->instrument);
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
    if (channels_.order_book) {
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

  [[nodiscard]] LighterBookSync* sync(InstrumentId id) noexcept {
    if (!id.valid() || id.value >= kMaxInstruments || index_[id.value] < 0) return nullptr;
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
  [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

 private:
  void rebuild_payloads() {
    payloads_.clear();
    for (const std::uint32_t m : market_ids_) {
      if (channels_.order_book) payloads_.push_back(subscription_frame(true, "order_book", m));
      if (channels_.ticker) payloads_.push_back(subscription_frame(true, "ticker", m));
      if (channels_.trades) payloads_.push_back(subscription_frame(true, "trade", m));
    }
  }

  const SymbolTable& symbols_;
  VenueId venue_;
  std::string_view log_name_ = "lighter";
  EventSink& sink_;
  InstrumentCallback requester_;
  MdChannels channels_;
  std::int64_t min_interval_;
  MarketMap markets_;
  LighterMdParser parser_;
  std::array<std::int16_t, kMaxInstruments> index_{};
  std::vector<std::unique_ptr<LighterBookSync>> syncs_;
  std::vector<std::string> names_;
  std::vector<std::uint32_t> market_ids_;
  std::vector<InstrumentId> ids_;
  std::vector<std::string> payloads_;
  std::string last_error_;
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
  MdFeedStats stats_;
};

static_assert(MarketDataFeed<LighterMdFeed>);

}  // namespace fastmm::venues::lighter
