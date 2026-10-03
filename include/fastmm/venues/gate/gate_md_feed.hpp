#pragma once
// GateMdFeed: the MarketDataFeed (feed.hpp) for Gate USDT perpetual futures. Owns the decoder,
// one GateBookSync per subscribed instrument and the market-data EventSink; runs on the venue's
// reactor thread and allocates nothing after add_instrument().
//
// Subscriptions (futures WS docs): one request per channel with every contract in its payload:
//   {"time":T,"channel":"futures.obu","event":"subscribe","payload":["ob.SYM.<level>",...]}
//   {"time":T,"channel":"futures.book_ticker","event":"subscribe","payload":["SYM",...]}
//   {"time":T,"channel":"futures.trades","event":"subscribe","payload":["SYM",...]}
//   {"time":T,"channel":"futures.tickers","event":"subscribe","payload":["SYM",...]}
// `time` is the request's Unix seconds; the connector stamps it when it sends (fill_time()). A
// book gap unsubscribes and subscribes the instrument's obu stream again, which is how Gate sends
// a fresh snapshot (gate_book_sync.hpp).
#include "fastmm/core/time.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/gate/gate_book_sync.hpp"
#include "fastmm/venues/gate/gate_md_parser.hpp"
#include "fastmm/venues/symbology.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fastmm::venues::gate {

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

class GateMdFeed {
 public:
  // `book_level` is the obu depth: 50 (pushed every 20 ms) or 400 (every 100 ms).
  GateMdFeed(const SymbolTable& symbols,
             VenueId venue,
             EventSink& sink,
             ResubscribeRequester requester,
             int book_level = 50,
             std::int64_t min_resubscribe_interval_ns = GateBookSync::kDefaultMinInterval)
      : symbols_(symbols),
        venue_(venue),
        sink_(sink),
        requester_(requester),
        level_(book_level),
        min_interval_(min_resubscribe_interval_ns),
        parser_(symbols, venue) {
    index_.fill(-1);
  }

  bool add_instrument(InstrumentId id) {
    if (!symbols_.contains(id) || id.value >= kMaxInstruments || index_[id.value] >= 0)
      return false;
    index_[id.value] = static_cast<std::int16_t>(syncs_.size());
    syncs_.push_back(std::make_unique<GateBookSync>(id, venue_, sink_, requester_, min_interval_));
    syncs_.back()->set_log_names(log_name_, symbols_.venue_symbol(id));
    ids_.push_back(id);
    rebuild_payloads();
    return true;
  }
  // The connector's name for the syncs' log lines; it must outlive the feed.
  void set_log_name(std::string_view venue) noexcept {
    log_name_ = venue;
    for (std::size_t i = 0; i < syncs_.size(); ++i)
      syncs_[i]->set_log_names(venue, symbols_.venue_symbol(ids_[i]));
  }
  [[nodiscard]] std::span<const InstrumentId> instruments() const noexcept { return ids_; }
  [[nodiscard]] int book_level() const noexcept { return level_; }

  // Reference data for the tickers channel (GateMdParser::set_funding).
  void set_funding(InstrumentId id, Duration interval, std::int64_t next_ms) noexcept {
    parser_.set_funding(id, interval, next_ms);
  }

  [[nodiscard]] std::string obu_stream(InstrumentId id) const {
    return "ob." + std::string(symbols_.venue_symbol(id)) + "." + std::to_string(level_);
  }
  // Control path (rare): unsubscribe + subscribe the obu stream to obtain a new snapshot. The
  // frames carry a placeholder time: fill_time() before sending.
  [[nodiscard]] std::vector<std::string> resubscribe_payloads(InstrumentId id) const {
    const std::string stream = obu_stream(id);
    return {R"({"time":0000000000,"channel":"futures.obu","event":"unsubscribe","payload":[")" +
                stream + R"("]})",
            R"({"time":0000000000,"channel":"futures.obu","event":"subscribe","payload":[")" +
                stream + R"("]})"};
  }
  // Writes the request's Unix seconds over the "0000000000" placeholder of a payload.
  static void fill_time(std::string& payload, std::int64_t unix_s) noexcept {
    const std::size_t p = payload.find("\"time\":0000000000");
    if (p == std::string::npos) return;
    char buf[11];
    std::int64_t v = unix_s;
    for (int i = 9; i >= 0; --i) {
      buf[i] = static_cast<char>('0' + v % 10);
      v /= 10;
    }
    payload.replace(p + 7, 10, buf, 10);
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
        GateBookSync* s = sync(h->instrument);
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
  // Placeholder times: the connector copies and fill_time()s them (on_md_open).
  [[nodiscard]] std::span<const std::string> subscription_payloads() const noexcept {
    return payloads_;
  }

  void on_timer(std::int64_t now_ns) noexcept {
    for (auto& s : syncs_) s->on_timer(now_ns);
  }

  [[nodiscard]] GateBookSync* sync(InstrumentId id) noexcept {
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
    auto frame = [&](const char* channel, bool obu) {
      std::string p = R"({"time":0000000000,"channel":")";
      p += channel;
      p += R"(","event":"subscribe","payload":[)";
      bool first = true;
      for (InstrumentId id : ids_) {
        if (!first) p += ',';
        first = false;
        p += '"';
        p += obu ? obu_stream(id) : std::string(symbols_.venue_symbol(id));
        p += '"';
      }
      p += "]}";
      payloads_.push_back(std::move(p));
    };
    frame("futures.obu", true);
    frame("futures.book_ticker", false);
    frame("futures.trades", false);
    frame("futures.tickers", false);
  }

  const SymbolTable& symbols_;
  VenueId venue_;
  std::string_view log_name_ = "gate";
  EventSink& sink_;
  ResubscribeRequester requester_;
  int level_;
  std::int64_t min_interval_;
  GateMdParser parser_;
  std::array<std::int16_t, kMaxInstruments> index_{};
  std::vector<std::unique_ptr<GateBookSync>> syncs_;
  std::vector<InstrumentId> ids_;
  std::vector<std::string> payloads_;
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
  MdFeedStats stats_;
};

static_assert(MarketDataFeed<GateMdFeed>);

}  // namespace fastmm::venues::gate
