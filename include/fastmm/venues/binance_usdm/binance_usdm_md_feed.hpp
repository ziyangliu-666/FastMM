#pragma once
// BinanceUsdmMdFeed: the MarketDataFeed (feed.hpp) for Binance USDⓈ-M futures --
// BasicBinanceMdFeed (binance/binance_md_feed_base.hpp) with the futures parser and pu depth
// chaining, plus the second connection this venue alone has.
//
// Two combined-stream connections, following the 2026-03-05 URL split ("Important WebSocket Change
// Notice"; legacy /ws and /stream URLs were decommissioned on 2026-04-23):
//   <root>/public/stream?streams=<sym>@depth@100ms/<sym>@bookTicker/...
//   <root>/market/stream?streams=<sym>@aggTrade/<perp>@markPrice@1s/...
// Both feed on_message(); only the public connection starts and stops the book syncs. The mark
// price stream ("Mark Price Stream": mark, index, funding rate, next funding time, every second)
// is subscribed for the instruments added with add_perpetual() and becomes a PerpStateMsg
// (venues/perp_state.hpp) on the market-data sink.
//
// Book sync ("How to manage a local order book correctly", USDⓈ-M): buffer deltas, GET
// /fapi/v1/depth, drop u < lastUpdateId, the first applied delta has U <= lastUpdateId <= u, and
// every later delta's pu equals the previous u (BinanceFuturesSyncTraits in book_syncer.hpp).
#include "fastmm/venues/binance/binance_md_feed_base.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_md_parser.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace fastmm::venues::binance_usdm {

using binance::MdFeedStats;
using binance::SnapshotRequester;
using UsdmDepthSync = binance::BasicBinanceDepthSync<BinanceFuturesSyncTraits>;

class BinanceUsdmMdFeed : public binance::BasicBinanceMdFeed<BinanceUsdmMdParser, UsdmDepthSync> {
 public:
  BinanceUsdmMdFeed(const SymbolTable& symbols,
                    VenueId venue,
                    EventSink& sink,
                    SnapshotRequester requester,
                    std::int64_t min_snapshot_interval_ns = UsdmDepthSync::kDefaultMinInterval)
      : BasicBinanceMdFeed(symbols, venue, sink, requester, min_snapshot_interval_ns) {}

  // Startup only, after add_instrument(id): subscribe `id`'s mark price stream and report
  // `funding_interval` with its funding rate.
  bool add_perpetual(InstrumentId id, Duration funding_interval) {
    if (sync(id) == nullptr) return false;
    if (std::find(perps_.begin(), perps_.end(), id) == perps_.end()) perps_.push_back(id);
    parser_.set_funding_interval(id, funding_interval);
    return true;
  }
  [[nodiscard]] Duration funding_interval(InstrumentId id) const noexcept {
    return parser_.funding_interval(id);
  }

  // "/public/stream?streams=btcusdt@depth@100ms/btcusdt@bookTicker"
  [[nodiscard]] std::string public_target() const {
    static constexpr std::array<const char*, 2> kPublic = {"@depth@100ms", "@bookTicker"};
    return stream_target_for("/public/stream", kPublic);
  }
  // "/market/stream?streams=btcusdt@aggTrade/btcusdt@markPrice@1s": aggTrade for every
  // instrument, markPrice for the perpetuals.
  [[nodiscard]] std::string market_target() const {
    static constexpr std::array<const char*, 1> kMarket = {"@aggTrade"};
    std::string t = stream_target_for("/market/stream", kMarket);
    for (InstrumentId id : perps_) {
      t += '/';
      t += symbols_.lower_symbol(id);
      t += "@markPrice@1s";
    }
    return t;
  }

 private:
  std::vector<InstrumentId> perps_;
};

static_assert(MarketDataFeed<BinanceUsdmMdFeed>);

}  // namespace fastmm::venues::binance_usdm
