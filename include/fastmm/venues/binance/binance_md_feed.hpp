#pragma once
// BinanceMdFeed: the MarketDataFeed (feed.hpp) for Binance Spot -- BasicBinanceMdFeed
// (binance_md_feed_base.hpp) with the Spot parser and U/u depth chaining, plus the SBE frame
// path this venue alone has.
//
// Stream selection (6.4): one combined-stream connection
//   json  /stream?streams=<sym>@depth@100ms/<sym>@bookTicker/<sym>@trade/...
//         (web-socket-streams.md: combined streams, lowercase symbols, 1024 streams per
//         connection), text frames through BinanceMdParser (simdjson);
//   sbe   /stream?streams=<sym>@depth/<sym>@bestBidAsk/<sym>@trade/... on the SBE stream host
//         (sbe-market-data-streams.md), binary frames through BinanceSbeMdParser.
// Both produce the same messages; the depth snapshot comes from REST (JSON) either way.
#include "fastmm/venues/binance/binance_md_feed_base.hpp"
#include "fastmm/venues/binance/binance_md_parser.hpp"
#include "fastmm/venues/binance/binance_sbe_md_parser.hpp"

namespace fastmm::venues::binance {

enum class MdFormat : std::uint8_t { Json = 0, Sbe = 1 };

class BinanceMdFeed : public BasicBinanceMdFeed<BinanceMdParser, BinanceDepthSync> {
 public:
  BinanceMdFeed(const SymbolTable& symbols,
                VenueId venue,
                EventSink& sink,
                SnapshotRequester requester,
                std::int64_t min_snapshot_interval_ns = BinanceDepthSync::kDefaultMinInterval,
                MdFormat format = MdFormat::Json)
      : BasicBinanceMdFeed(symbols, venue, sink, requester, min_snapshot_interval_ns),
        format_(format),
        sbe_(symbols, venue) {}

  [[nodiscard]] MdFormat format() const noexcept { return format_; }

  // json: "/stream?streams=btcusdt@depth@100ms/btcusdt@bookTicker/btcusdt@trade"
  // sbe:  "/stream?streams=btcusdt@depth/btcusdt@bestBidAsk/btcusdt@trade"
  [[nodiscard]] std::string stream_target(std::string_view base_path = "/stream") const {
    static constexpr std::array<const char*, 3> kJson = {"@depth@100ms", "@bookTicker", "@trade"};
    static constexpr std::array<const char*, 3> kSbe = {"@depth", "@bestBidAsk", "@trade"};
    return stream_target_for(base_path,
                             format_ == MdFormat::Sbe ? std::span(kSbe) : std::span(kJson));
  }

  // One SBE binary frame (md_format = "sbe"). A trade frame can yield several TradeMsgs.
  ParseStatus on_binary(std::span<const std::byte> frame, std::int64_t rx_ts) noexcept {
    ++stats_.messages;
    const Cycles t0 = rdtscp();
    const Timestamp recv = wall_now();
    bool overflow = false;
    const ParseStatus st =
        sbe_.decode(frame, recv, t0, scratch_, [&](EventHeader& h, MdKind kind) noexcept {
          h.t1_delta = static_cast<std::uint32_t>(rdtscp() - t0);
          if (kind == MdKind::BookDelta) {
            if (BinanceDepthSync* s = sync(h.instrument)) {
              s->on_delta(*reinterpret_cast<const BookDeltaMsg*>(&h), rx_ts);
              ++stats_.pushed;
            }
            return;
          }
          // depth<N> partial books are not used for synchronisation (diff + REST snapshot is).
          if (kind == MdKind::BookSnapshot) return;
          if (!sink_.push(h)) {
            ++stats_.dropped;
            overflow = true;
            return;
          }
          ++stats_.pushed;
        });
    switch (st) {
      case ParseStatus::Ok:
        break;
      case ParseStatus::Malformed:
        ++stats_.malformed;
        break;
      case ParseStatus::UnknownSymbol:
        ++stats_.unknown_symbol;
        break;
      default:
        ++stats_.ignored;
        break;
    }
    return overflow ? ParseStatus::Overflow : st;
  }

  [[nodiscard]] const MdParserStats& parser_stats() const noexcept {
    return format_ == MdFormat::Sbe ? sbe_.stats() : BasicBinanceMdFeed::parser_stats();
  }

 private:
  MdFormat format_;
  BinanceSbeMdParser sbe_;
};

static_assert(MarketDataFeed<BinanceMdFeed>);

}  // namespace fastmm::venues::binance
