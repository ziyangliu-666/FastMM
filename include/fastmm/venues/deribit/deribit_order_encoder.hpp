#pragma once
// Deribit order entry and session control over the authenticated WebSocket (JSON-RPC 2.0), plus
// the REST kill-switch request. Method and parameter names from the OpenAPI spec
// (https://docs.deribit.com/specifications/deribit_openapi.json, checked 2026-09-14):
//
//   private/buy | private/sell  instrument_name, contracts (instead of amount), type limit|market,
//                               price (limit), label (<= 64 chars: the 14-char FastMM client id),
//                               time_in_force good_til_cancelled|fill_or_kill|immediate_or_cancel,
//                               post_only (documented default true, so always sent explicitly),
//                               reject_post_only (post-only orders: reject instead of repricing),
//                               reduce_only
//   private/edit                order_id, contracts, price, post_only, reject_post_only. The new
//                               amount is the total including fills ("the remaining quantity is
//                               updated": order-management article), like the engine's replace qty
//   private/cancel              order_id
//   private/cancel_by_label     label, currency (cancels before the order id is known)
//   private/cancel_all_by_instrument  instrument_name (kill switch, channel loss)
//   private/get_open_orders_by_currency  currency (reconciliation)
//   private/enable_cancel_on_disconnect  scope connection
//   private/subscribe, public/subscribe, public/set_heartbeat (interval >= 10 s: the testnet
//   answers -32602 "value must be >= 10"), public/test, public/auth
//
// The access token goes in params.access_token of every private request (authentication article,
// "Access token placement"). Request ids: orders use the string "<n|c|r><cl_ord_id>" (the testnet
// echoes string ids unchanged), control requests small integers (DeribitVenue).
//
// Quantities: FastMM Qty is contracts (Instrument::contract_multiplier = Deribit contract_size),
// sent as `contracts`. Prices are rounded passively (buys down, sells up) to the instrument's
// tick schedule: tick_size, replaced by tick_size_steps[].tick_size at and above
// tick_size_steps[].above_price (BTC options: 0.0001, 0.0005 from 0.005), so an order is never
// rejected with 10043 price_wrong_tick.
//
// REST (kill switch, independent of the reactor): GET <rest_url>/private/cancel_all_by_instrument?
// instrument_name=NAME with "Authorization: Basic base64(client_id:client_secret)" (authentication
// article, "Basic User Credentials").
#include "fastmm/core/containers/static_vector.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/deribit/deribit_rest_decoder.hpp"
#include "fastmm/venues/order_commands.hpp"
#include "fastmm/venues/request_id.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace fastmm::venues::deribit {

inline constexpr std::size_t kMaxRequestBytes = 4096;
inline constexpr std::size_t kMaxTickSteps = 4;

struct Credentials {
  std::string client_id;
  Secret<std::string> client_secret{};
  [[nodiscard]] bool usable() const noexcept {
    return !client_id.empty() && !client_secret.value.empty();
  }
};

// Price grid of one instrument: `base` below the first step, each step's tick from its
// above_price upwards (steps ascending).
struct TickSchedule {
  Price base{};
  StaticVector<TickStep, kMaxTickSteps> steps;

  [[nodiscard]] Price tick_for(Price px) const noexcept {
    Price t = base;
    for (const TickStep& s : steps) {
      if (px >= s.above_price && s.tick.is_positive()) t = s.tick;
    }
    return t;
  }
  // Passive rounding onto the grid that applies at the rounded price.
  [[nodiscard]] Price round(Price px, Side side) const noexcept {
    const Price t = tick_for(px);
    if (!t.is_positive()) return px;
    Price out = round_to_tick(px, t, side);
    // A buy rounded down below a step boundary lands on the finer grid, which is still valid; a
    // sell rounded up past a boundary may need the coarser tick there.
    const Price t2 = tick_for(out);
    if (t2 != t) out = round_to_tick(out, t2, side);
    return out;
  }
};

// What an edit/cancel needs that the engine message does not carry. `label` is the client id the
// venue knows the order by: after an edit the engine's id changes but the label stays.
struct OrderShadow {
  InstrumentId instrument{};
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  TimeInForce tif = TimeInForce::Gtc;
  ClientOrderId label{};
  ClientOrderId replaces{};  // Replace: the engine order this one edits
  VenueOrderId venue_order_id{};
  Qty qty{};     // total contracts
  Qty filled{};  // contracts filled so far (user.trades)
  // An edit of this order is in flight: user.orders "open" updates for its label are not acked
  // until the edit response, so the OMS never sees the old id acked while it waits for the new one.
  bool edit_pending = false;
};

class DeribitOrderEncoder {
 public:
  DeribitOrderEncoder(const SymbolTable& symbols,
                      const InstrumentTable& instruments,
                      std::span<const TickSchedule> ticks,
                      bool reject_post_only = true) noexcept
      : symbols_(symbols),
        instruments_(instruments),
        ticks_(ticks),
        reject_post_only_(reject_post_only) {}

  // Order request for `cmd` (bytes written; 0 = not encodable, e.g. an edit without an order id,
  // or buffer overflow). `shadow` is required for Replace and used for Cancel.
  std::size_t encode(const OrderCommand& cmd,
                     const OrderShadow* shadow,
                     std::string_view access_token,
                     std::span<char> out) const noexcept;

  [[nodiscard]] Price venue_price(InstrumentId id, Price px, Side side) const noexcept;

  // ---- session control ---------------------------------------------------------------------
  static std::size_t encode_auth(std::int64_t id,
                                 std::string_view client_id,
                                 std::string_view client_secret,
                                 std::span<char> out) noexcept;
  static std::size_t encode_auth_refresh(std::int64_t id,
                                         std::string_view refresh_token,
                                         std::span<char> out) noexcept;
  static std::size_t encode_set_heartbeat(std::int64_t id,
                                          std::int64_t interval_s,
                                          std::span<char> out) noexcept;
  static std::size_t encode_test(std::int64_t id, std::span<char> out) noexcept;
  static std::size_t encode_subscribe(std::int64_t id,
                                      bool is_private,
                                      std::span<const std::string> channels,
                                      std::string_view access_token,
                                      std::span<char> out) noexcept;
  static std::size_t encode_enable_cancel_on_disconnect(std::int64_t id,
                                                        std::string_view access_token,
                                                        std::span<char> out) noexcept;
  static std::size_t encode_open_orders(std::int64_t id,
                                        std::string_view currency,
                                        std::string_view access_token,
                                        std::span<char> out) noexcept;
  static std::size_t encode_cancel_all_by_instrument(std::int64_t id,
                                                     std::string_view instrument,
                                                     std::string_view access_token,
                                                     std::span<char> out) noexcept;

  // ---- REST kill switch (control path, allocation allowed) ---------------------------------
  [[nodiscard]] static std::string rest_cancel_all_target(std::string_view instrument);
  // "Authorization: Basic base64(client_id:client_secret)\r\n"
  [[nodiscard]] static std::string basic_auth_header(const Credentials& c);

  [[nodiscard]] static constexpr std::string_view tif_text(TimeInForce t) noexcept {
    switch (t) {
      case TimeInForce::Ioc:
        return "immediate_or_cancel";
      case TimeInForce::Fok:
        return "fill_or_kill";
      case TimeInForce::Day:
        return "good_til_day";
      case TimeInForce::Gtc:
        return "good_til_cancelled";
    }
    return "good_til_cancelled";
  }

 private:
  const SymbolTable& symbols_;
  const InstrumentTable& instruments_;
  std::span<const TickSchedule> ticks_;
  bool reject_post_only_;
};

}  // namespace fastmm::venues::deribit
