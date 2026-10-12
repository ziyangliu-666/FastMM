#pragma once
// Hyperliquid order entry: actions, their signatures and the frames that carry them.
//
// Actions (exchange endpoint docs, read 2026-10-12; the field order is the SDK's, which the
// signature's msgpack covers):
//   order          {"type":"order","orders":[{"a":asset,"b":isBuy,"p":"px","s":"sz","r":reduceOnly,
//                   "t":{"limit":{"tif":"Alo"|"Gtc"|"Ioc"}},"c":"0x<32 hex>"?}],"grouping":"na"}
//   cancel         {"type":"cancel","cancels":[{"a":asset,"o":oid}]}
//   cancelByCloid  {"type":"cancelByCloid","cancels":[{"asset":asset,"cloid":"0x.."}]}
//   batchModify    {"type":"batchModify","modifies":[{"oid":oid|"0x<cloid>","order":{..}}]}
//   scheduleCancel {"type":"scheduleCancel","time":ms?}  (no time: the scheduled cancel is removed)
// Carried as {"action":..,"nonce":ms,"signature":{"r","s","v"},"vaultAddress":null,
// "expiresAfter":null}: the body of POST /exchange, or the payload of a WebSocket post
//   {"method":"post","id":N,"request":{"type":"action","payload":{..}}}.
// Prices: at most 5 significant figures and at most 6 - szDecimals decimals (an integer price is
// always allowed); sizes: at most szDecimals decimals. Both go on the wire the way the SDK's
// float_to_wire writes them: no trailing zeros, no trailing point.
// Client order ids (cloid): 16 bytes as "0x" and 32 hex digits; FastMM's are
// 0x666d000000000000 ("fm") followed by the 64-bit ClientOrderId.
#include "fastmm/core/fixed_string.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/venues/hyperliquid/hl_crypto.hpp"
#include "fastmm/venues/order_commands.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace fastmm::venues::hyperliquid {

using Cloid = FixedString<40>;
using WireDecimal = FixedString<32>;

inline constexpr std::size_t kMaxRequestBytes = std::size_t{16} * 1024;
// Orders or cancels in one action: the venue counts each against the address budget and a
// batch of n as 1 + floor(n / 40) of IP weight.
inline constexpr std::size_t kMaxBatch = 40;

[[nodiscard]] Cloid cloid_of(ClientOrderId id) noexcept;
// nullopt for a cloid FastMM did not make (another client's, or none).
[[nodiscard]] std::optional<ClientOrderId> cl_ord_id_of_cloid(std::string_view cloid) noexcept;

// Decimal places a price of `px` may carry on an asset with `sz_decimals`.
[[nodiscard]] int price_decimals(Price px, int sz_decimals) noexcept;
// The price step at `px`: 10^-price_decimals.
[[nodiscard]] Price tick_at(Price px, int sz_decimals) noexcept;
// `px` on the venue's price grid, rounded down (bids) or up (asks): never more aggressive.
[[nodiscard]] Price round_price(Price px, int sz_decimals, bool down) noexcept;
// `q` rounded down to szDecimals.
[[nodiscard]] Qty round_size(Qty q, int sz_decimals) noexcept;
// The SDK's float_to_wire text of a fixed-point value: "230.32", "86287", "0.001", "0".
[[nodiscard]] WireDecimal wire_text(std::int64_t raw) noexcept;
template <class Tag>
[[nodiscard]] WireDecimal wire_text(Fixed<Tag> v) noexcept {
  return wire_text(v.raw);
}

struct OrderWire {
  std::uint32_t asset = 0;
  bool buy = true;
  WireDecimal px;
  WireDecimal sz;
  bool reduce_only = false;
  std::string_view tif = "Gtc";  // "Alo" | "Gtc" | "Ioc"
  Cloid cloid;                   // empty: none
};
struct CancelWire {
  std::uint32_t asset = 0;
  std::uint64_t oid = 0;
};
struct CancelCloidWire {
  std::uint32_t asset = 0;
  Cloid cloid;
};
struct ModifyWire {
  std::uint64_t oid = 0;  // 0: the order is named by `by_cloid`
  Cloid by_cloid;
  OrderWire order;
};

enum class ActionKind : std::uint8_t { Order, Cancel, CancelByCloid, BatchModify, ScheduleCancel };

// One action, written as msgpack (what is signed) and as JSON (what is sent) side by side.
class ActionBuilder {
 public:
  static constexpr std::size_t kMsgpackBytes = std::size_t{8} * 1024;
  static constexpr std::size_t kJsonBytes = std::size_t{12} * 1024;

  bool order(std::span<const OrderWire> orders) noexcept;
  bool cancel(std::span<const CancelWire> cancels) noexcept;
  bool cancel_by_cloid(std::span<const CancelCloidWire> cancels) noexcept;
  bool batch_modify(std::span<const ModifyWire> modifies) noexcept;
  // time_ms <= 0: remove the scheduled cancel.
  bool schedule_cancel(std::int64_t time_ms) noexcept;

  [[nodiscard]] std::span<const std::uint8_t> msgpack() const noexcept {
    return {msgpack_, msgpack_len_};
  }
  [[nodiscard]] std::string_view json() const noexcept { return {json_, json_len_}; }
  [[nodiscard]] ActionKind kind() const noexcept { return kind_; }

 private:
  bool finish(bool ok, std::size_t mp, std::size_t js, ActionKind k) noexcept;
  std::uint8_t msgpack_[kMsgpackBytes];
  char json_[kJsonBytes];
  std::size_t msgpack_len_ = 0;
  std::size_t json_len_ = 0;
  ActionKind kind_ = ActionKind::Order;
};

// Signs actions and writes the frames that carry them. Nonces: the wall clock's milliseconds,
// raised past the last one so they strictly increase per signer (the venue keeps the 100 highest
// and refuses a nonce below them or a repeat).
class ActionSigner {
 public:
  // `signer` must outlive this object.
  ActionSigner(const Signer& signer, bool mainnet, std::string vault_address = {}) noexcept
      : signer_(signer), mainnet_(mainnet), vault_(std::move(vault_address)) {}
  ActionSigner(Signer&&, bool, std::string = {}) = delete;

  // Any thread (the kill switch signs from its own).
  [[nodiscard]] std::uint64_t next_nonce(std::int64_t wall_ms) noexcept {
    const auto w = static_cast<std::uint64_t>(wall_ms > 0 ? wall_ms : 0);
    std::uint64_t last = last_nonce_.load(std::memory_order_relaxed);
    std::uint64_t next = 0;
    do {
      next = w > last ? w : last + 1;
    } while (!last_nonce_.compare_exchange_weak(last, next, std::memory_order_relaxed));
    return next;
  }
  [[nodiscard]] std::uint64_t last_nonce() const noexcept {
    return last_nonce_.load(std::memory_order_relaxed);
  }
  // The payload object {"action","nonce","signature","vaultAddress","expiresAfter"}; 0 on failure
  // (unusable signer, overflow).
  std::size_t write_payload(const ActionBuilder& a,
                            std::uint64_t nonce,
                            std::span<char> out) const noexcept;
  // {"method":"post","id":<id>,"request":{"type":"action","payload":<payload>}}
  std::size_t write_post(const ActionBuilder& a,
                         std::uint64_t nonce,
                         std::uint64_t post_id,
                         std::span<char> out) const noexcept;
  [[nodiscard]] bool mainnet() const noexcept { return mainnet_; }

 private:
  const Signer& signer_;
  bool mainnet_;
  std::string vault_;
  std::atomic<std::uint64_t> last_nonce_{0};
};

// {"method":"post","id":<id>,"request":{"type":"info","payload":<payload>}}
std::size_t write_info_post(std::string_view payload,
                            std::uint64_t post_id,
                            std::span<char> out) noexcept;
// {"method":"subscribe"|"unsubscribe","subscription":{"type":<type>,"coin":<coin>}} or with
// "user" instead of "coin".
std::size_t write_subscription(bool subscribe,
                               std::string_view type,
                               std::string_view field,
                               std::string_view value,
                               std::span<char> out) noexcept;
inline constexpr std::string_view kPingFrame = R"({"method":"ping"})";

}  // namespace fastmm::venues::hyperliquid
