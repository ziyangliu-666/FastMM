#pragma once
// Hyperliquid replies: the answer to an action (a WebSocket post or POST /exchange) and the info
// queries the connector makes (POST /info, or a post of type "info").
//
//   post reply   {"channel":"post","data":{"id":N,"response":{"type":"action"|"info",
//                 "payload":..}}}; an unparseable request gets {"channel":"error","data":".."}
//   action       {"status":"ok","response":{"type":"order"|"cancel"|"default",
//                 "data":{"statuses":[..]}}} or {"status":"err","response":"<text>"}
//     statuses   {"resting":{"oid":N,"cloid"?}} | {"filled":{"totalSz","avgPx","oid","cloid"?}} |
//                {"error":"<text>"} | "success"
//   info         {"type":<query>,"data":<the query's answer>}
//
// The info decoders (control path) read the answers of perpDexs, meta / metaAndAssetCtxs,
// clearinghouseState, frontendOpenOrders, userFillsByTime, userRateLimit and orderStatus into
// plain rows; an error string names the first thing that did not read.
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/hyperliquid/hl_order_encoder.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::hyperliquid {

struct OrderStatus {
  enum class Kind : std::uint8_t { Resting, Filled, Error, Success, Other };
  Kind kind = Kind::Other;
  std::uint64_t oid = 0;
  std::string_view cloid;
  std::string_view error;
  Qty total_sz{};
  Price avg_px{};
};

struct PostReply {
  enum class Kind : std::uint8_t { None, Action, Info, Error };
  Kind kind = Kind::None;
  std::uint64_t id = 0;
  bool ok = false;               // status "ok"
  std::string_view error;        // status "err": the text; channel "error": its data
  std::string_view action_type;  // response.type: order | cancel | default
  std::size_t count = 0;         // statuses
  OrderStatus statuses[kMaxBatch];
  std::string_view info_type;  // info: payload.type
  std::string_view info_data;  // info: payload.data as raw JSON
};

class HlResponseDecoder {
 public:
  explicit HlResponseDecoder(std::size_t capacity = 1U << 20);
  ~HlResponseDecoder();
  HlResponseDecoder(const HlResponseDecoder&) = delete;
  HlResponseDecoder& operator=(const HlResponseDecoder&) = delete;

  // A frame of the order-entry WebSocket: Ok for a post reply or an error, Ignored for anything
  // else (pong, subscription acks). Views point into `json` (padded).
  ParseStatus decode_frame(std::string_view json, PostReply& out) noexcept;
  // The body of POST /exchange: the action payload alone.
  ParseStatus decode_exchange(std::string_view json, PostReply& out) noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---- info answers -------------------------------------------------------------------------------

struct AssetMeta {
  std::string name;  // "BTC", "xyz:NVDA"
  int sz_decimals = 0;
  int max_leverage = 0;
  bool only_isolated = false;  // onlyIsolated, or marginMode noCross / strictIsolated
  bool delisted = false;
  Price mark{};  // metaAndAssetCtxs: markPx (zero when absent)
};
struct PositionRow {
  std::string coin;
  Qty szi{};  // signed
  Price entry{};
};
struct ClearinghouseState {
  std::vector<PositionRow> positions;
  Notional account_value{};
  Notional margin_used{};
  Notional withdrawable{};
  std::int64_t time_ms = 0;
};
// spotClearinghouseState: a unified account keeps its perp collateral here (USDC), and the
// per-dex clearinghouseState then shows accountValue 0.
struct SpotBalanceRow {
  std::string coin;
  Notional total{};
  Notional hold{};
};
struct OpenOrderRow {
  std::string coin;
  bool buy = true;
  Price px{};
  Qty sz{};       // left
  Qty orig_sz{};  // placed
  std::uint64_t oid = 0;
  std::string cloid;
  std::int64_t time_ms = 0;
};
struct FillRow {
  std::string coin;
  Price px{};
  Qty sz{};
  bool buy = true;
  std::int64_t time_ms = 0;
  std::uint64_t oid = 0;
  std::uint64_t tid = 0;
  bool crossed = false;
  Notional fee{};
  std::string fee_token;
  std::string cloid;
};
struct RateLimitState {
  double cum_volume = 0;  // USDC traded since inception
  std::int64_t used = 0;
  std::int64_t cap = 0;
  std::int64_t surplus = 0;
};
struct OrderStatusRow {
  std::string status;  // "unknownOid" when the venue does not know it
  std::int64_t status_ms = 0;
  std::int64_t placed_ms = 0;  // order.timestamp
};

// perpDexs: [null, {"name":"xyz",..}, ..] -> names by perp dex index ("" for the main dex).
std::string decode_perp_dexs(std::string_view body, std::vector<std::string>& out);
// meta: {"universe":[..]} or metaAndAssetCtxs: [{"universe":[..]},[ctx..]].
std::string decode_meta(std::string_view body, std::vector<AssetMeta>& out);
std::string decode_clearinghouse(std::string_view body, ClearinghouseState& out);
std::string decode_spot_balances(std::string_view body, std::vector<SpotBalanceRow>& out);
std::string decode_open_orders(std::string_view body, std::vector<OpenOrderRow>& out);
std::string decode_user_fills(std::string_view body, std::vector<FillRow>& out);
std::string decode_rate_limit(std::string_view body, RateLimitState& out);
std::string decode_order_status(std::string_view body, OrderStatusRow& out);

}  // namespace fastmm::venues::hyperliquid
