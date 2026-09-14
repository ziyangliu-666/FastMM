#pragma once
// Deribit JSON-RPC frame decoder for the public market-data connection
// (wss://test.deribit.com/ws/api/v2). One WebSocket text frame becomes normalised messages written
// back to back into the caller's scratch buffer, or a control classification:
//
//   {"method":"subscription","params":{"channel":C,"data":D}}   (JSON-RPC notification,
//    https://docs.deribit.com/articles/json-rpc-overview, "Notification Messages")
//     book.NAME.INTERVAL    D {type snapshot|change, change_id, prev_change_id (not on the
//                           snapshot), timestamp, bids/asks [[action new|change|delete, price,
//                           amount]]}  -> BookDeltaMsg (first = last = change_id, prev =
//                           prev_change_id; snapshot: BookSnapshot + kSnapshot)
//     ticker.NAME.INTERVAL  D {best_bid_price|null, best_bid_amount, best_ask_price|null,
//                           best_ask_amount, mark_price, underlying_price, index_price, mark_iv,
//                           bid_iv, ask_iv, interest_rate, greeks {delta, gamma, vega, theta,
//                           rho}, timestamp} -> BookTickerMsg, plus OptionTickerMsg for options
//     trades.NAME.INTERVAL  D [{trade_id, trade_seq, timestamp, price, amount, direction}]
//                           -> one TradeMsg per trade
//   {"method":"heartbeat","params":{"type":"test_request"|"heartbeat"}}   (public/set_heartbeat)
//   {"id":..,"result":..} / {"id":..,"error":{"code","message","data"}}  -> response
//
// Channel and field names:
// https://docs.deribit.com/subscriptions/orderbook/bookinstrument_nameinterval,
// .../market-data/tickerinstrument_nameinterval, .../trades/tradesinstrument_nameinterval and the
// AsyncAPI spec (deribit_asyncapi.json), checked 2026-09-14 against recorded testnet frames.
//
// Units: book and trade `amount` is USD for perpetuals and inverse futures and the base coin for
// options and linear futures (docs); FastMM quantities are contracts, so amounts are divided by
// the instrument's contract_multiplier (Deribit contract_size: 10 USD for BTC-PERPETUAL, 1 BTC
// for BTC options). IVs arrive in percent and are stored as decimals. The trade `direction` is
// taken as the taker (aggressor) side; the current docs only say "Direction: buy, or sell".
//
// Snapshots carry every price level (no depth limit); more than kMaxBookLevelsPerMsg levels per
// side are truncated to the best ones (stats.truncated_snapshots). simdjson On-Demand lives in the
// .cpp; frames need kJsonPadding readable bytes behind them. No allocation after construction.
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace fastmm::venues::deribit {

struct MdParserStats {
  std::uint64_t frames = 0;
  std::uint64_t book_snapshots = 0;
  std::uint64_t book_changes = 0;
  std::uint64_t book_tickers = 0;
  std::uint64_t option_tickers = 0;
  std::uint64_t trades = 0;
  std::uint64_t responses = 0;
  std::uint64_t heartbeats = 0;
  std::uint64_t ignored = 0;
  std::uint64_t malformed = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t overflow = 0;
  std::uint64_t truncated_snapshots = 0;
};

enum class FrameKind : std::uint8_t {
  Other = 0,
  Notification = 1,  // "method":"subscription"
  Response = 2,      // has "id"
  Heartbeat = 3,     // informational heartbeat
  TestRequest = 4,   // must be answered with public/test
};

// JSON-RPC response fields shared by the public and private decoders.
struct RpcHeader {
  std::int64_t id = -1;      // numeric id, -1 if absent or a string
  std::string_view id_text;  // string id (order requests), empty otherwise
  bool is_error = false;
  std::int64_t error_code = 0;
  std::string_view error_message;
  std::string_view error_reason;  // error.data.reason
};

struct MdDecodeResult : DecodeResult {
  std::uint32_t count = 0;  // messages written back to back
  FrameKind frame = FrameKind::Other;
  RpcHeader rpc;                   // FrameKind::Response
  std::uint32_t result_items = 0;  // length of an array result (subscribe/unsubscribe)
};

class DeribitMdParser {
 public:
  static constexpr std::size_t kDefaultCapacity = 4U << 20;

  DeribitMdParser(const SymbolTable& symbols,
                  const InstrumentTable& instruments,
                  VenueId venue,
                  std::size_t capacity = kDefaultCapacity);
  ~DeribitMdParser();
  DeribitMdParser(const DeribitMdParser&) = delete;
  DeribitMdParser& operator=(const DeribitMdParser&) = delete;

  // `out` must hold kDecoderScratchBytes. Messages carry recv_ts/t0 from the arguments.
  MdDecodeResult decode(std::string_view json,
                        Timestamp recv_ts,
                        Cycles t0,
                        std::span<std::byte> out) noexcept;

  [[nodiscard]] const MdParserStats& stats() const noexcept { return stats_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  const SymbolTable& symbols_;
  const InstrumentTable& instruments_;
  VenueId venue_;
  MdParserStats stats_;
};

// Amount in venue units -> contracts (amount / contract_size, exact in fixed point). Identity for
// a multiplier of 1 or a non-positive one.
[[nodiscard]] constexpr Qty amount_to_contracts(Qty amount, Qty contract_size) noexcept {
  if (contract_size.raw == kFixedScale || contract_size.raw <= 0) return amount;
  return Qty::from_raw(
      static_cast<std::int64_t>(static_cast<Int128>(amount.raw) * kFixedScale / contract_size.raw));
}
[[nodiscard]] constexpr Qty contracts_to_amount(Qty contracts, Qty contract_size) noexcept {
  if (contract_size.raw == kFixedScale || contract_size.raw <= 0) return contracts;
  return Qty::from_raw(static_cast<std::int64_t>(static_cast<Int128>(contracts.raw) *
                                                 contract_size.raw / kFixedScale));
}

// Trade ids are decimal strings on the testnet ("267258393"); anything else is folded to 64 bits
// with FNV-1a so it still dedupes.
[[nodiscard]] constexpr std::uint64_t trade_id_of(std::string_view s) noexcept {
  if (!s.empty() && s.size() <= 19) {
    std::uint64_t v = 0;
    bool numeric = true;
    for (char c : s) {
      if (c < '0' || c > '9') {
        numeric = false;
        break;
      }
      v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (numeric) return v;
  }
  std::uint64_t h = 14695981039346656037ULL;
  for (char c : s) {
    h ^= static_cast<std::uint8_t>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

}  // namespace fastmm::venues::deribit
