#pragma once
// BybitMdFeed: the MarketDataFeed (feed.hpp) for Bybit v5 spot. Owns the decoder, one
// BybitBookSync per subscribed instrument and the market-data EventSink; runs on the venue's
// reactor thread and allocates nothing after add_instrument().
//
// Subscriptions (https://bybit-exchange.github.io/docs/v5/ws/connect, "How to Subscribe to
// Topics"): {"req_id":..,"op":"subscribe","args":[...]}, at most 10 args per request on
// spot. Topics per instrument: orderbook.<depth>.SYM, orderbook.1.SYM, publicTrade.SYM.
#include "fastmm/core/time.hpp"
#include "fastmm/venues/bybit/bybit_book_sync.hpp"
#include "fastmm/venues/bybit/bybit_md_parser.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/symbology.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fastmm::venues::bybit {

inline constexpr std::size_t kMaxArgsPerSubscribe = 10;  // spot limit (connect page)

struct MdFeedStats {
  std::uint64_t messages = 0;
  std::uint64_t pushed = 0;
  std::uint64_t dropped = 0;
  std::uint64_t malformed = 0;
  std::uint64_t ignored = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t subscribe_errors = 0;
  std::uint64_t pongs = 0;
};

class BybitMdFeed {
 public:
  BybitMdFeed(const SymbolTable& symbols,
              VenueId venue,
              EventSink& sink,
              ResubscribeRequester requester,
              int depth = 50,
              std::int64_t min_resubscribe_interval_ns = BybitBookSync::kDefaultMinInterval)
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
    index_[id.value] = static_cast<std::int16_t>(syncs_.size());
    syncs_.push_back(std::make_unique<BybitBookSync>(id, venue_, sink_, requester_, min_interval_));
    ids_.push_back(id);
    rebuild_payloads();
    return true;
  }
  [[nodiscard]] std::span<const InstrumentId> instruments() const noexcept { return ids_; }
  [[nodiscard]] int depth() const noexcept { return depth_; }

  [[nodiscard]] std::vector<std::string> topics(InstrumentId id) const {
    const std::string sym(symbols_.venue_symbol(id));
    return {"orderbook." + std::to_string(depth_) + "." + sym,
            "orderbook.1." + sym,
            "publicTrade." + sym};
  }
  // Control path (rare): unsubscribe + subscribe the depth topic to obtain a new snapshot.
  [[nodiscard]] std::vector<std::string> resubscribe_payloads(InstrumentId id) const {
    const std::string topic =
        "orderbook." + std::to_string(depth_) + "." + std::string(symbols_.venue_symbol(id));
    return {R"({"req_id":"unsub","op":"unsubscribe","args":[")" + topic + R"("]})",
            R"({"req_id":"resub","op":"subscribe","args":[")" + topic + R"("]})"};
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
        if (r.control == ControlOp::Subscribe) ++stats_.subscribe_errors;  // venue logs it
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
        BybitBookSync* s = sync(h->instrument);
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
    parser_.reset_tickers();
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

  [[nodiscard]] BybitBookSync* sync(InstrumentId id) noexcept {
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
  void rebuild_payloads() {
    payloads_.clear();
    std::vector<std::string> args;
    for (InstrumentId id : ids_) {
      for (std::string& t : topics(id)) args.push_back(std::move(t));
    }
    for (std::size_t i = 0; i < args.size(); i += kMaxArgsPerSubscribe) {
      std::string p = R"({"req_id":"md)" + std::to_string(i / kMaxArgsPerSubscribe) +
                      R"(","op":"subscribe","args":[)";
      for (std::size_t k = i; k < args.size() && k < i + kMaxArgsPerSubscribe; ++k) {
        if (k != i) p += ',';
        p += '"';
        p += args[k];
        p += '"';
      }
      p += "]}";
      payloads_.push_back(std::move(p));
    }
  }

  const SymbolTable& symbols_;
  VenueId venue_;
  EventSink& sink_;
  ResubscribeRequester requester_;
  int depth_;
  std::int64_t min_interval_;
  BybitMdParser parser_;
  std::array<std::int16_t, kMaxInstruments> index_{};
  std::vector<std::unique_ptr<BybitBookSync>> syncs_;
  std::vector<InstrumentId> ids_;
  std::vector<std::string> payloads_;
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
  MdFeedStats stats_;
};

static_assert(MarketDataFeed<BybitMdFeed>);

}  // namespace fastmm::venues::bybit
