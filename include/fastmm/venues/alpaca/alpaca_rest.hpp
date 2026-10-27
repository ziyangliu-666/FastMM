#pragma once
// Alpaca Trading API v2 (https://docs.alpaca.markets/reference, read 2026-10-27; every reply shape
// below was also seen on the paper account), base https://paper-api.alpaca.markets/v2. Every
// request carries APCA-API-KEY-ID and APCA-API-SECRET-KEY headers.
//
//   POST   /v2/orders {"symbol","qty","side","type":"limit"|"market","time_in_force",
//          "limit_price","client_order_id","extended_hours"} -> the order
//   PATCH  /v2/orders/<id> {"qty","limit_price","client_order_id"} -> the replacement, a new
//          order with its own id ("replaces": <id>); the old one ends "replaced"
//   DELETE /v2/orders/<id> -> 204, also for an order that is over; 404 for an unknown id
//   GET    /v2/orders?status=open&symbols=A,B&limit=500&direction=asc -> [order..]
//   GET    /v2/orders/<id> -> the order
//   GET    /v2/positions -> [{"symbol","qty","side":"long"|"short","avg_entry_price",..}]
//   GET    /v2/account/activities/FILL?after=&until=&direction=desc&page_size=&page_token= ->
//          [{"id":"<ET time>::<execution_id>","transaction_time","price","qty","side",
//          "symbol","order_id","cum_qty","leaves_qty",..}], newest first, paged by id
//   GET    /v2/assets/<symbol> -> {"status","tradable","shortable","fractionable",..}
//   GET    /v2/clock -> {"timestamp","is_open","next_open","next_close"}
//   errors: an HTTP status with {"code":40310000,"message":..,..}
//
// trade_updates (wss://paper-api.alpaca.markets/stream; the paper stream sends binary frames):
//   {"stream":"trade_updates","data":{"event","execution_id","timestamp","price","qty",
//    "position_qty","order":{..}}}; events new, fill, partial_fill, canceled, expired,
//   done_for_day, replaced, rejected, pending_*, order_cancel_rejected, order_replace_rejected.
//   {"stream":"authorization","data":{"status":"authorized"|"unauthorized",..}},
//   {"stream":"listening","data":{"streams":[..]}}.
//
// FastMM's client order id ("fm" + 12 hex digits) is the client_order_id as it is. The execution
// id of a fill is the stream's execution_id and the part of the activity id after "::", so a fill
// replayed from the activities is the one the stream reported.
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/venues/error_action.hpp"
#include "fastmm/venues/order_commands.hpp"
#include "fastmm/venues/symbology.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::alpaca {

inline constexpr std::string_view kPrefix = "/v2";

// One REST order request written into fixed buffers.
struct OrderRequest {
  std::array<char, 96> path{};
  std::size_t path_n = 0;
  std::array<char, 320> body{};
  std::size_t body_n = 0;
  [[nodiscard]] std::string_view target() const noexcept { return {path.data(), path_n}; }
  [[nodiscard]] std::string_view payload() const noexcept { return {body.data(), body_n}; }
};

enum class Tif : std::uint8_t { Day, Gtc };

struct EncoderOptions {
  Tif resting_tif = Tif::Day;   // for the engine's Gtc and Day orders
  bool extended_hours = false;  // "extended_hours":true on resting limit orders
};

class AlpacaOrderEncoder {
 public:
  AlpacaOrderEncoder(const SymbolTable& symbols, EncoderOptions opts) noexcept
      : symbols_(symbols), opts_(opts) {}

  // POST /v2/orders for a New. PostOnly goes as a plain limit: Alpaca has no post-only flag.
  bool encode_new(const OrderCommand& cmd, OrderRequest& out) const noexcept;
  // PATCH /v2/orders/<venue_id> for a Replace: the new price, quantity and client id.
  static bool encode_replace(const OrderCommand& cmd,
                             std::string_view venue_id,
                             OrderRequest& out) noexcept;
  static std::string order_path(std::string_view venue_id);
  static std::string open_orders_path(std::span<const std::string> symbols);
  static std::string fills_path(std::int64_t after_ms,
                                std::int64_t until_ms,
                                std::string_view page_token,
                                int page_size);

 private:
  const SymbolTable& symbols_;
  EncoderOptions opts_;
};

// Views into the frame (trade_updates) or owned copies (REST replies).
template <class S>
struct BasicOrder {
  S id;
  S client_order_id;
  S symbol;
  S side;    // buy | sell
  S status;  // new, partially_filled, filled, canceled, replaced, ...
  S replaced_by;
  Qty qty{};
  Qty filled_qty{};
  Price limit_price{};
};
using OrderView = BasicOrder<std::string_view>;
using OrderRow = BasicOrder<std::string>;

struct TradeUpdate {
  enum class Stream : std::uint8_t { Other, Authorization, Listening, TradeUpdates };
  Stream stream = Stream::Other;
  std::string_view status;  // authorization: authorized | unauthorized
  std::string_view event;
  std::string_view execution_id;
  std::int64_t time_ns = 0;  // "timestamp"
  Price price{};             // fill: this execution
  Qty qty{};
  OrderView order;
};

// Allocation free after construction; `json` needs kJsonPadding readable bytes behind it. False:
// malformed.
class TradeUpdateParser {
 public:
  TradeUpdateParser();
  ~TradeUpdateParser();
  TradeUpdateParser(const TradeUpdateParser&) = delete;
  TradeUpdateParser& operator=(const TradeUpdateParser&) = delete;
  bool parse(std::string_view json, TradeUpdate& out) noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct PositionRow {
  std::string symbol;
  Qty qty{};  // signed: short is negative
  Price avg_px{};
};

struct FillRow {
  std::string exec_id;  // the activity id after "::"
  std::string order_id;
  std::string symbol;
  Side side = Side::Buy;  // sell and sell_short are sells
  Price price{};
  Qty qty{};
  std::int64_t time_ms = 0;
  std::string page_id;  // the whole activity id: the next page's token
};

struct AssetInfo {
  std::string status;  // active | inactive
  bool tradable = false;
  bool shortable = false;
  bool fractionable = false;
};

struct ApiError {
  std::int64_t code = 0;
  std::string message;
  std::string reject_reason;
};

// Empty string on success, else an error description.
std::string decode_order(std::string_view json, OrderRow& out);
std::string decode_orders(std::string_view json, std::vector<OrderRow>& out);
std::string decode_positions(std::string_view json, std::vector<PositionRow>& out);
std::string decode_fills(std::string_view json, std::vector<FillRow>& out);
std::string decode_asset(std::string_view json, AssetInfo& out);
std::string decode_clock(std::string_view json, std::int64_t& epoch_ms, bool& is_open);
// {"code","message"}; code 0 and an empty message when the body is not one.
ApiError decode_error(std::string_view json);

// HTTP status and Alpaca code -> RejectReason and VenueAction.
[[nodiscard]] constexpr ErrorMapping map_error(int status,
                                               std::int64_t code,
                                               std::string_view msg) noexcept {
  if (status >= 200 && status < 300) return {RejectReason::None, VenueAction::None, true};
  if (status == 429) return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
  if (status == 401 || code == 40110000)
    return {RejectReason::VenueReject, VenueAction::Fatal, true};
  if (status == 403) {
    if (contains_ci(msg, "wash trade") || contains_ci(msg, "short sell while a long buy"))
      return {RejectReason::SelfTradePrevention, VenueAction::None, true};
    if (contains_ci(msg, "insufficient"))
      return {RejectReason::InsufficientBalance, VenueAction::None, true};
    if (contains_ci(msg, "not authorized") || contains_ci(msg, "forbidden"))
      return {RejectReason::VenueReject, VenueAction::Fatal, true};
    return {RejectReason::VenueReject, VenueAction::None, code != 0};
  }
  if (status == 404) return {RejectReason::VenueUnknownOrder, VenueAction::None, true};
  if (status == 422) {
    if (contains_ci(msg, "sub-penny") || contains_ci(msg, "limit_price"))
      return {RejectReason::InvalidTick, VenueAction::DisableInstrument, true};
    if (contains_ci(msg, "client_order_id must be unique"))
      return {RejectReason::DuplicateId, VenueAction::None, true};
    if (contains_ci(msg, "not open") || contains_ci(msg, "not found"))
      return {RejectReason::VenueUnknownOrder, VenueAction::None, true};
    if (contains_ci(msg, "qty") || contains_ci(msg, "fractional"))
      return {RejectReason::InvalidLot, VenueAction::DisableInstrument, true};
    if (contains_ci(msg, "not active") || contains_ci(msg, "not tradable"))
      return {RejectReason::InstrumentDisabled, VenueAction::DisableInstrument, true};
    return {RejectReason::VenueReject, VenueAction::None, code != 0};
  }
  if (status >= 500) return {RejectReason::VenueReject, VenueAction::Backoff, true};
  return {RejectReason::VenueReject, VenueAction::None, false};
}

}  // namespace fastmm::venues::alpaca
