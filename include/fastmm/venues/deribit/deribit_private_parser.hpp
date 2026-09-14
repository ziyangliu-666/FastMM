#pragma once
// Deribit private-connection decoder: user channel notifications, JSON-RPC responses to order and
// session requests, and heartbeats, in one parse per frame.
//
// Notifications (AsyncAPI spec deribit_asyncapi.json, checked 2026-09-14):
//   user.orders.KIND.CURRENCY.raw  data: order object (or an array of them for aggregated
//                                  intervals) {order_id, order_state, label, instrument_name,
//                                  direction, price, amount, contracts?, filled_amount,
//                                  last_update_timestamp, cancel_reason}
//       order_state open        -> OrderAckMsg (duplicates are ignored by the OMS)
//                   cancelled   -> OrderCancelAckMsg (cum_qty = filled contracts)
//                   rejected    -> OrderRejectMsg
//                   filled, untriggered, triggered -> nothing (fills come from user.trades)
//   user.trades.KIND.CURRENCY.I    data: [{trade_id, trade_seq, order_id, label, instrument_name,
//                                  direction, price, amount, contracts?, fee, liquidity M|T,
//                                  state, timestamp}] -> OrderFillMsg (exec_id = trade_id; cum_qty
//                                  and leaves_qty 0: DeribitVenue fills them from its shadow)
// Responses: result.order (private/buy, private/sell, private/edit), result = order
// (private/cancel), result.access_token/refresh_token/expires_in (public/auth), array results
// (subscribe) and integer results (cancel_all_*).
//
// Client ids come from `label`; labels that are not FastMM ids yield an invalid ClientOrderId
// (the OMS treats the order as unknown). Amounts are converted to contracts with the instrument's
// contract_multiplier (Deribit contract_size); `contracts` is used when present.
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/deribit/deribit_md_parser.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

namespace fastmm::venues::deribit {

struct PrivateParserStats {
  std::uint64_t frames = 0;
  std::uint64_t orders = 0;
  std::uint64_t trades = 0;
  std::uint64_t responses = 0;
  std::uint64_t heartbeats = 0;
  std::uint64_t ignored = 0;
  std::uint64_t malformed = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t foreign_ids = 0;
  std::uint64_t overflow = 0;
};

// result.order / result of an order request.
struct OrderResult {
  bool present = false;
  std::string_view order_id;
  std::string_view order_state;
  std::string_view label;
  std::string_view instrument_name;
  Qty amount{};  // venue units
  Qty filled_amount{};
};

struct AuthResult {
  bool present = false;
  std::string_view access_token;
  std::string_view refresh_token;
  std::int64_t expires_in = 0;  // seconds
};

struct PrivateDecodeResult : DecodeResult {
  std::uint32_t count = 0;  // order events written back to back
  FrameKind frame = FrameKind::Other;
  RpcHeader rpc;
  OrderResult order;
  AuthResult auth;
  std::uint32_t result_items = 0;  // array result length
  std::int64_t result_int = -1;    // integer result (cancel_all_*), -1 otherwise
};

struct OpenOrderRecord {
  std::string_view instrument_name;
  std::string_view order_id;
  std::string_view label;
  std::string_view direction;
  std::string_view order_state;
  Price price{};
  Qty amount{};         // venue units
  Qty filled_amount{};  // venue units
};

class DeribitPrivateParser {
 public:
  DeribitPrivateParser(const SymbolTable& symbols,
                       const InstrumentTable& instruments,
                       VenueId venue,
                       std::size_t capacity = 1U << 20);
  ~DeribitPrivateParser();
  DeribitPrivateParser(const DeribitPrivateParser&) = delete;
  DeribitPrivateParser& operator=(const DeribitPrivateParser&) = delete;

  // `out` must hold kDecoderScratchBytes. Views in the result point into `json` or the parser's
  // string buffer and stay valid until the next decode.
  PrivateDecodeResult decode(std::string_view json,
                             Timestamp recv_ts,
                             Cycles t0,
                             std::span<std::byte> out) noexcept;

  // private/get_open_orders_by_currency response (control path). Error for an error response.
  ParseStatus decode_open_orders(std::string_view json,
                                 const std::function<void(const OpenOrderRecord&)>& fn) noexcept;

  [[nodiscard]] const PrivateParserStats& stats() const noexcept { return stats_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  const SymbolTable& symbols_;
  const InstrumentTable& instruments_;
  VenueId venue_;
  PrivateParserStats stats_;
};

}  // namespace fastmm::venues::deribit
