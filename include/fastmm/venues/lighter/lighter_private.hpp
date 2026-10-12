#pragma once
// Lighter account streams, transaction replies and REST bodies (the WebSocket and REST references
// read 2026-10-13; shapes recorded from production, tests/fixtures/lighter):
//
//   account_all_orders/{account}  (auth token) {"channel":"account_all_orders:N","orders":
//       {"<market>":[Order..]},"type":..}; Order {order_index, client_order_index, market_index,
//       price, initial_base_amount, remaining_base_amount, filled_base_amount, is_ask, status,
//       transaction_time (us), ..}; status open | filled | canceled | canceled-<why> | pending |
//       in-progress
//   account_all_trades/{account}  {"trades":{"<market>":[Trade..]},"type":..}; Trade {trade_id,
//       market_id, price, size, usd_amount, ask_id, bid_id (order indexes), ask_client_id,
//       bid_client_id, ask_account_id, bid_account_id, is_maker_ask, taker_fee, maker_fee (rates
//       in millionths of the notional; absent when zero), timestamp (ms)}; the subscription's
//       first frame (subscribed/..) is the account's history and is not decoded
//   jsonapi/sendtx(batch) replies  {"error":{"code","message"},"id"} when refused (recorded); an
//       accepted one carries code 200, tx_hash, predicted_execution_time_ms and
//       volume_quota_remaining (RespSendTx, the REST reply's shape), found at the top level or
//       under "data"
//   REST: orderBookDetails, nextNonce, account, apikeys, accountActiveOrders, trades
//
// Rows hold views into the parsed text: valid until the parser's next decode.
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/feed.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::lighter {

// What an order's status means.
enum class StatusClass : std::uint8_t {
  Ignore,    // pending, in-progress, an unknown word
  Open,      // resting
  Filled,    // done by fills (the fills come on account_all_trades)
  Canceled,  // cancelled on request, by a cancel-all or the scheduled cancel
  Ended,     // ended by the venue: canceled-post-only, -margin-not-allowed, -expired, ...
};
[[nodiscard]] StatusClass classify_status(std::string_view s) noexcept;
// The reject reason of an order the venue ended before it rested.
[[nodiscard]] RejectReason ended_reason(std::string_view status) noexcept;

struct OrderRow {
  std::int64_t market = -1;
  std::int64_t order_index = 0;
  std::int64_t client_order_index = 0;
  Price price{};
  Qty initial{};
  Qty remaining{};
  Qty filled{};
  bool is_ask = false;
  std::string_view status;
  std::int64_t time_us = 0;  // transaction_time
};

struct FillRow {
  std::int64_t market = -1;
  std::uint64_t trade_id = 0;
  Price price{};
  Qty size{};
  Notional usd{};
  Side side = Side::Buy;  // the account's side
  bool maker = false;
  std::int64_t fee_rate = 0;  // millionths of the notional
  std::int64_t order_index = 0;
  std::int64_t client_order_index = 0;
  std::int64_t time_ms = 0;
  bool self_trade_other_leg = false;  // the second row of a trade with itself
};

// The fee of a fill: usd * rate / 10^6.
[[nodiscard]] Notional fee_of(Notional usd, std::int64_t rate) noexcept;

struct TxReply {
  std::string_view id;  // the request's id
  std::int64_t code = 0;
  std::string_view message;
  bool refused = false;  // an "error" object, or a code other than 200
  std::int64_t quota_remaining = -1;
  std::int64_t predicted_ms = -1;
};

enum class FrameKind : std::uint8_t {
  Ignored,  // connected, pong, a subscription's history, a channel not read
  Orders,
  Trades,
  TxReply,
  Error,  // an error with no id (a refused subscription)
  Malformed,
};

struct PrivateFrame {
  FrameKind kind = FrameKind::Ignored;
  std::string_view type;
  std::string_view channel;
  TxReply reply;  // TxReply and Error
};

class LighterPrivateParser {
 public:
  explicit LighterPrivateParser(std::int64_t account, std::size_t capacity = 1U << 20);
  ~LighterPrivateParser();
  LighterPrivateParser(const LighterPrivateParser&) = delete;
  LighterPrivateParser& operator=(const LighterPrivateParser&) = delete;

  // A frame of the private or the trade connection; `json` needs kJsonPadding readable bytes
  // behind it. Rows go to orders() / fills().
  PrivateFrame decode(std::string_view json) noexcept;

  [[nodiscard]] const std::vector<OrderRow>& orders() const noexcept { return orders_; }
  [[nodiscard]] const std::vector<FillRow>& fills() const noexcept { return fills_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::int64_t account_;
  std::vector<OrderRow> orders_;
  std::vector<FillRow> fills_;
};

// ---- REST (control path) ----------------------------------------------------------------------

struct MarketDetail {
  std::int64_t market_id = -1;
  std::string symbol;
  std::string market_type;  // "perp" | "spot"
  bool active = false;
  int size_decimals = 0;
  int price_decimals = 0;
  Qty min_base{};
  Notional min_quote{};
};

struct PositionRow {
  std::int64_t market = -1;
  Qty qty{};  // signed
  Price avg_entry{};
};

struct AccountState {
  std::int64_t index = 0;
  Notional collateral{};
  Notional available{};
  Notional equity{};  // total_asset_value: collateral with the positions' unrealised PnL
  std::vector<PositionRow> positions;
};

struct ApiKeyRow {
  std::int64_t api_key_index = -1;
  std::int64_t nonce = 0;
  std::string public_key;
};

// Each: "" on success, else what is wrong (a venue error names its code and message).
std::string decode_market_details(std::string_view body, std::vector<MarketDetail>& out);
std::string decode_next_nonce(std::string_view body, std::int64_t& nonce);
std::string decode_account(std::string_view body, AccountState& out);
std::string decode_api_keys(std::string_view body, std::vector<ApiKeyRow>& out);
// accountActiveOrders: {"code":200,"orders":[Order..]}.
std::string decode_active_orders(std::string_view body, std::vector<OrderRow>& out);
// trades: {"code":200,"next_cursor","trades":[Trade..]}, the account's side of each.
std::string decode_trades(std::string_view body,
                          std::int64_t account,
                          std::vector<FillRow>& out,
                          std::string& next_cursor);
// A sendTx / sendTxBatch reply body.
std::string decode_send_tx(std::string_view body, TxReply& out);

// What a refused transaction means for the order and the connector (codes and texts as the
// venue words them; the API docs publish no list).
struct TxErrorMapping {
  RejectReason reason = RejectReason::VenueReject;
  bool rate_limited = false;
  bool nonce = false;  // the key's nonce is off: read it again
  bool fatal = false;  // key, account or signature: no order can succeed
  bool unknown_order = false;
};
[[nodiscard]] TxErrorMapping map_tx_error(std::int64_t code, std::string_view message) noexcept;

}  // namespace fastmm::venues::lighter
