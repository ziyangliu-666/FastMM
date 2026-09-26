#pragma once
// OKX v5 order encoding and response decoding, USDT-margined perpetual swaps (instType SWAP).
//
// Primary path: WebSocket order operations on wss://ws.okx.com:8443/ws/v5/private after `op:
// login` (https://www.okx.com/docs-v5/en/#order-book-trading-trade-ws-place-order, ...-ws-amend-
// order, ...-ws-cancel-order; read 2026-09-26):
//   {"id":"<n|c|r><cl_ord_id>","op":"order"|"amend-order"|"cancel-order","args":[{...}]}
//   response {"id","op","code","msg","data":[{"ordId","clOrdId","tag","ts","sCode","sMsg",
//             "subCode","reqId"}],"inTime","outTime"}
// `id`: "alphanumerics ... up to 32 characters"; FastMM's is 15. The instrument is `instIdCode`
// (an integer from GET /api/v5/public/instruments): since 2026-03-26 `order` ignores `instId`, and
// since 2026-04-07 so do `amend-order` and `cancel-order`.
// Order args: instIdCode, tdMode (cross | isolated), clOrdId (up to 32 alphanumerics, unique among
// the account's pending orders; FastMM's is 14), side buy | sell, ordType limit | post_only | ioc
// | fok | market, px (not for market), sz (contracts), reduceOnly (boolean, net mode). posSide is
// left out: net mode takes "net" or nothing, and the connector refuses long/short mode.
// Amend: instIdCode, ordId | clOrdId, reqId (the engine's new id), newSz ("the amount that has
// been filled" included, so the total, as the engine's replace qty is), newPx.
// Cancel: instIdCode, ordId | clOrdId.
//
// REST (signed per okx_auth.hpp over the exact bytes sent; still keyed by `instId`):
//   POST /api/v5/trade/order | amend-order | cancel-order    order-entry fallback
//   POST /api/v5/trade/cancel-batch-orders                    at most 20 orders per request
//   POST /api/v5/trade/cancel-all-after                       {"timeOut":"<0 | 10..120>"}
//   GET  /api/v5/trade/orders-pending?instType=SWAP&limit=100[&after=<ordId>]
//   GET  /api/v5/trade/fills |
//   fills-history?instType=SWAP&begin=..[&end=..][&after=<billId>]&limit=100 GET
//   /api/v5/account/bills | bills-archive?instType=SWAP&type=8&begin=..[&after=..]&limit=100 GET
//   /api/v5/account/positions?instType=SWAP GET  /api/v5/account/config
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/okx/okx_auth.hpp"
#include "fastmm/venues/order_commands.hpp"
#include "fastmm/venues/request_id.hpp"
#include "fastmm/venues/symbology.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace fastmm::venues::okx {

inline constexpr std::size_t kMaxRequestBytes = 1024;
// cancel-all-after: "0" disables it, otherwise 10 to 120 seconds.
inline constexpr int kMinCancelAfterS = 10;
inline constexpr int kMaxCancelAfterS = 120;
// cancel-batch-orders and batch-orders: "Maximum 20 orders can be ... per request".
inline constexpr std::size_t kMaxBatch = 20;

enum class TdMode : std::uint8_t { Cross = 0, Isolated = 1 };
[[nodiscard]] constexpr std::string_view to_string(TdMode m) noexcept {
  return m == TdMode::Isolated ? "isolated" : "cross";
}

// Per working order: what an amend or cancel needs that the engine message does not carry.
// `link_id` is the clOrdId the venue knows the order by: an amend keeps it while the engine's id
// changes.
struct OrderShadow {
  InstrumentId instrument{};
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  TimeInForce tif = TimeInForce::Gtc;
  ClientOrderId link_id{};
  ClientOrderId replaces{};  // Replace: the order this one amends
};

struct RestRequest {
  std::string_view method;  // "GET" | "POST"
  std::string path;         // with the query string: the signed requestPath
  std::string body;         // POST: signed with the path
  bool is_order = false;
};

class OkxOrderEncoder {
 public:
  OkxOrderEncoder(const SymbolTable& symbols, TdMode td_mode = TdMode::Cross) noexcept
      : symbols_(symbols), td_mode_(td_mode) {
    codes_.fill(-1);
  }

  // instIdCode per instrument (reference data); -1 when the venue gave none, which sends that
  // instrument's orders over REST.
  void set_inst_id_code(InstrumentId id, std::int64_t code) noexcept {
    if (id.value < codes_.size()) codes_[id.value] = code;
  }
  [[nodiscard]] std::int64_t inst_id_code(InstrumentId id) const noexcept {
    return id.value < codes_.size() ? codes_[id.value] : -1;
  }
  [[nodiscard]] TdMode td_mode() const noexcept { return td_mode_; }

  // ---- WebSocket frames (bytes written; 0 = unsupported / overflow / no instIdCode) --------
  std::size_t encode_ws(const OrderCommand& cmd,
                        const OrderShadow* shadow,  // required for Replace; optional otherwise
                        std::span<char> out) const noexcept;
  // {"op":"login","args":[{"apiKey","passphrase","timestamp","sign"}]}
  static std::size_t encode_login(const Signer& signer,
                                  std::int64_t unix_s,
                                  std::span<char> out) noexcept;
  // {"id":id,"op":"subscribe","args":[{"channel":C,"instType":"SWAP"},..]}
  static std::size_t encode_private_subscribe(std::string_view id,
                                              std::span<const std::string_view> channels,
                                              std::span<char> out) noexcept;

  // ---- REST --------------------------------------------------------------------------------
  bool encode_rest(const OrderCommand& cmd, const OrderShadow* shadow, RestRequest& out) const;
  struct CancelEntry {
    std::string_view inst_id;
    std::string_view ord_id;
  };
  static bool encode_rest_cancel_batch(std::span<const CancelEntry> orders, RestRequest& out);
  static bool encode_rest_cancel_all_after(int timeout_s, RestRequest& out);
  // `after`: the ordId of the previous page's last order (empty for the first).
  static void encode_rest_orders_pending(std::string_view after, RestRequest& out);
  // GET /api/v5/trade/fills (the last 3 days) or fills-history (3 months), newest first. `begin_ms`
  // and `end_ms` (0 = open) filter on `ts`; `after` is the previous page's last billId.
  static void encode_rest_fills(bool history,
                                std::int64_t begin_ms,
                                std::int64_t end_ms,
                                std::string_view after,
                                int limit,
                                RestRequest& out);
  // GET /api/v5/account/bills (7 days) or bills-archive (3 months), type 8 (funding fee).
  static void encode_rest_funding_bills(
      bool archive, std::int64_t begin_ms, std::string_view after, int limit, RestRequest& out);
  static void encode_rest_positions(RestRequest& out);
  static void encode_rest_account_config(RestRequest& out);

  [[nodiscard]] static std::string_view side_text(Side s) noexcept {
    return s == Side::Buy ? "buy" : "sell";
  }
  [[nodiscard]] static std::string_view ord_type_text(OrderType type, TimeInForce t) noexcept {
    if (type == OrderType::Market) return "market";
    if (type == OrderType::PostOnly) return "post_only";
    switch (t) {
      case TimeInForce::Ioc:
        return "ioc";
      case TimeInForce::Fok:
        return "fok";
      case TimeInForce::Gtc:
      case TimeInForce::Day:
        return "limit";
    }
    return "limit";
  }

 private:
  // The args object; `ws` names the instrument by instIdCode, REST by instId.
  std::size_t write_args(const OrderCommand& cmd,
                         const OrderShadow* shadow,
                         bool ws,
                         std::span<char> out) const noexcept;

  const SymbolTable& symbols_;
  TdMode td_mode_;
  std::array<std::int64_t, kMaxInstruments> codes_{};
};

// ---- responses -------------------------------------------------------------------------------

// A WebSocket order-operation reply or a REST order reply (the same envelope), or an event on the
// order connection (login, error).
struct TradeResponse {
  std::string_view id;
  std::string_view op;
  std::string_view event;  // "login" | "error" for events
  int code = -1;           // top-level
  std::string_view msg;
  std::string_view ord_id;
  std::string_view cl_ord_id;
  std::string_view req_id;
  int s_code = -1;  // data[0].sCode; the top-level code when data is empty
  std::string_view s_msg;
  [[nodiscard]] bool ok() const noexcept { return code == 0 && s_code == 0; }
  // The code that says why: the order's own, else the request's.
  [[nodiscard]] int reason_code() const noexcept { return s_code > 0 ? s_code : code; }
  [[nodiscard]] std::string_view reason_msg() const noexcept {
    return !s_msg.empty() ? s_msg : msg;
  }
};

class OkxResponseDecoder {
 public:
  explicit OkxResponseDecoder(std::size_t capacity = 1U << 20);
  ~OkxResponseDecoder();
  OkxResponseDecoder(const OkxResponseDecoder&) = delete;
  OkxResponseDecoder& operator=(const OkxResponseDecoder&) = delete;

  // Views point into `json` (padded). Ignored for "pong"; Malformed for anything unreadable.
  ParseStatus decode(std::string_view json, TradeResponse& out) noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fastmm::venues::okx
