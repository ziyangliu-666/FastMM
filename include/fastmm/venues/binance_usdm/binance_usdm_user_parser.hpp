#pragma once
// Binance USDⓈ-M futures user data stream decoder ("User Data Streams": `ORDER_TRADE_UPDATE`,
// `ACCOUNT_UPDATE`, `listenKeyExpired`). One frame -> zero or more messages written back to back.
//
//   ORDER_TRADE_UPDATE o.x  NEW                   -> OrderAckMsg
//                           CANCELED              -> OrderCancelAckMsg (cum = z)
//                           EXPIRED               -> OrderExpiredMsg (cum = z; GTX that would take,
//                                                    IOC remainder, self-trade prevention)
//                           TRADE, CALCULATED     -> OrderFillMsg (qty l, price L, cum z,
//                                                    leaves q - z, fee n in asset N, maker m)
//                           AMENDMENT and others  -> Ignored (order.modify is acknowledged from
//                                                    its response)
//   ACCOUNT_UPDATE a.P[]    ps == "BOTH"          -> PositionUpdateMsg (qty pa, avg ep) per known
//                                                    symbol; LONG/SHORT (hedge mode) are counted
//                                                    and skipped
//                  a.m      FUNDING_FEE           -> `funding` set: the connector books the
//                                                    payment from GET /fapi/v1/income (the event
//                                                    has no id to deduplicate it by)
//   listenKeyExpired                              -> Ignored with listen_key_expired set
//
// Client ids that FastMM did not mint are ignored, except fills (liquidations "autoclose-*", ADL,
// manual trades): the engine books every fill into the position. Field names from the official
// connector's generated models (binance-connector-python, derivatives_trading_usds_futures
// websocket_streams models OrderTradeUpdateO, AccountUpdateAPInner), since the documentation page
// embeds the schema. A funding fee arrives as an ACCOUNT_UPDATE with reason FUNDING_FEE, the symbol
// in a.S and the balance change in B[].bc, and no transaction id ("User Data Streams", read
// 2026-09-26).
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace fastmm::venues::binance_usdm {

struct UserParserStats {
  std::uint64_t frames = 0;
  std::uint64_t order_updates = 0;
  std::uint64_t account_updates = 0;
  std::uint64_t positions = 0;
  std::uint64_t hedge_positions = 0;  // LONG/SHORT entries (hedge mode is not supported)
  std::uint64_t ignored = 0;
  std::uint64_t malformed = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t foreign_ids = 0;
  std::uint64_t listen_key_expired = 0;
  std::uint64_t funding_events = 0;  // ACCOUNT_UPDATE with reason FUNDING_FEE
};

struct UserDecodeResult : DecodeResult {
  std::uint32_t count = 0;          // messages written back to back in `out`
  bool listen_key_expired = false;  // the stream stops until a new listenKey is used
  bool funding = false;             // an ACCOUNT_UPDATE for a funding payment
};

class BinanceUsdmUserParser {
 public:
  BinanceUsdmUserParser(const SymbolTable& symbols,
                        const InstrumentTable& instruments,
                        VenueId venue,
                        std::size_t capacity = 1U << 20);
  ~BinanceUsdmUserParser();
  BinanceUsdmUserParser(const BinanceUsdmUserParser&) = delete;
  BinanceUsdmUserParser& operator=(const BinanceUsdmUserParser&) = delete;

  UserDecodeResult decode(std::string_view json,
                          Timestamp recv_ts,
                          Cycles t0,
                          std::span<std::byte> out) noexcept;

  [[nodiscard]] const UserParserStats& stats() const noexcept { return stats_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  const SymbolTable& symbols_;
  const InstrumentTable& instruments_;
  VenueId venue_;
  UserParserStats stats_;
};

}  // namespace fastmm::venues::binance_usdm
