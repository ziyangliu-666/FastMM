#pragma once
// Lighter L2 transactions: their hash (the message the API key signs) and their tx_info JSON, as
// lighter-go's types/txtypes write them (create_order.go, cancel_order.go, modify_order.go,
// cancel_all_orders.go, change_pub_key.go, tx_attributes.go):
//
//   hash = Poseidon2 over [chain id, tx type, nonce, expiredAt, account, api key, fields...]
//          (lighter_crypto.hpp); when the tx carries attributes, Poseidon2(hash || attrHash)
//          with attrHash over the sorted (type, value) pairs, padded with (0, 0) to four
//   tx_info = the Go struct's json.Marshal: fields in declaration order, Sig base64, the
//          attributes as {"<type>":value} or null
//
//   CreateOrder 14  [.., market, client order index, base amount, price, is ask, type, tif,
//                    reduce only, trigger price, order expiry]
//   CancelOrder 15  [.., market, index]           index: a client order index (< 2^48) or the
//   ModifyOrder 17  [.., market, index, base amount, price, trigger price]   venue's order index
//   CancelAll   16  [.., tif, time]               tif 0 now, 1 scheduled at `time`, 2 abort it
//   ChangePubKey 8  [.., pubkey as five field elements]; the L1 key signs a text body as well
//
// Amounts are integers: base amount = size * 10^size_decimals, price = price *
// 10^price_decimals (uint32), both of the market (orderBookDetails).
#include "fastmm/venues/lighter/lighter_crypto.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace fastmm::venues::lighter {

inline constexpr std::uint32_t kMainnetChainId = 304;
inline constexpr std::uint32_t kTestnetChainId = 300;

enum TxType : std::uint8_t {
  kTxChangePubKey = 8,
  kTxCreateOrder = 14,
  kTxCancelOrder = 15,
  kTxCancelAll = 16,
  kTxModifyOrder = 17,
};

enum OrderTypeCode : std::uint8_t { kOrderLimit = 0, kOrderMarket = 1 };
enum TifCode : std::uint8_t { kTifIoc = 0, kTifGoodTillTime = 1, kTifPostOnly = 2 };
enum CancelAllTif : std::uint8_t {
  kCancelAllNow = 0,
  kCancelAllScheduled = 1,
  kCancelAllAbort = 2
};

enum AttributeType : std::uint8_t {
  kAttrSkipNonce = 4,             // value 1: the tx does not take the next nonce
  kAttrCancelAllMarketIndex = 5,  // an immediate cancel-all of one market (nil 255)
};

inline constexpr std::int64_t kMaxClientOrderIndex = (std::int64_t{1} << 48) - 1;
inline constexpr std::int64_t kMinCancelAllWindowMs = 5LL * 60 * 1000;
inline constexpr std::int64_t kMaxCancelAllWindowMs = 15LL * 24 * 3600 * 1000;
// What SignerClient leaves as the default: a resting order lives 28 days, a tx 10 minutes.
inline constexpr std::int64_t kDefaultOrderExpiryMs = 28LL * 24 * 3600 * 1000;
inline constexpr std::int64_t kDefaultTxExpiryMs = 10LL * 60 * 1000 - 1000;

// At most four (type, value) attributes, the venue's NbAttributesPerTx.
struct TxAttributes {
  struct Attr {
    std::uint8_t type = 0;
    std::int64_t value = 0;
  };
  std::array<Attr, 4> items{};
  std::uint8_t count = 0;
  bool add(std::uint8_t type, std::int64_t value) noexcept;
  [[nodiscard]] bool empty() const noexcept { return count == 0; }
};

struct TxHeader {
  std::int64_t account_index = 0;
  std::uint8_t api_key_index = 0;
  std::int64_t expired_at = 0;  // ms
  std::int64_t nonce = 0;
  TxAttributes attributes;
};

struct CreateOrderTx {
  TxHeader h;
  std::int16_t market_index = 0;
  std::int64_t client_order_index = 0;
  std::int64_t base_amount = 0;
  std::uint32_t price = 0;
  std::uint8_t is_ask = 0;
  std::uint8_t type = kOrderLimit;
  std::uint8_t time_in_force = kTifPostOnly;
  std::uint8_t reduce_only = 0;
  std::uint32_t trigger_price = 0;
  std::int64_t order_expiry = 0;  // ms; 0 for IOC
};

struct CancelOrderTx {
  TxHeader h;
  std::int16_t market_index = 0;
  std::int64_t index = 0;
};

struct ModifyOrderTx {
  TxHeader h;
  std::int16_t market_index = 0;
  std::int64_t index = 0;
  std::int64_t base_amount = 0;
  std::uint32_t price = 0;
  std::uint32_t trigger_price = 0;
};

struct CancelAllTx {
  TxHeader h;
  std::uint8_t time_in_force = kCancelAllNow;
  std::int64_t time = 0;  // ms, for kCancelAllScheduled
};

struct ChangePubKeyTx {
  TxHeader h;
  Fp5Bytes pub_key{};
  std::string l1_sig;  // "0x" + 130 hex digits, the account's L1 key over l1_body()
};

[[nodiscard]] Fp5Bytes tx_hash(const CreateOrderTx& tx, std::uint32_t chain_id) noexcept;
[[nodiscard]] Fp5Bytes tx_hash(const CancelOrderTx& tx, std::uint32_t chain_id) noexcept;
[[nodiscard]] Fp5Bytes tx_hash(const ModifyOrderTx& tx, std::uint32_t chain_id) noexcept;
[[nodiscard]] Fp5Bytes tx_hash(const CancelAllTx& tx, std::uint32_t chain_id) noexcept;
[[nodiscard]] Fp5Bytes tx_hash(const ChangePubKeyTx& tx, std::uint32_t chain_id) noexcept;

// The text the account's Ethereum key signs (EIP-191 personal_sign) to register an API key.
[[nodiscard]] std::string l1_body(const ChangePubKeyTx& tx);

// tx_info into `out`; the length written, 0 when it does not fit.
inline constexpr std::size_t kMaxTxInfoBytes = 768;
std::size_t write_tx_info(const CreateOrderTx& tx,
                          const SigBytes& sig,
                          std::span<char> out) noexcept;
std::size_t write_tx_info(const CancelOrderTx& tx,
                          const SigBytes& sig,
                          std::span<char> out) noexcept;
std::size_t write_tx_info(const ModifyOrderTx& tx,
                          const SigBytes& sig,
                          std::span<char> out) noexcept;
std::size_t write_tx_info(const CancelAllTx& tx, const SigBytes& sig, std::span<char> out) noexcept;
std::size_t write_tx_info(const ChangePubKeyTx& tx,
                          const SigBytes& sig,
                          std::span<char> out) noexcept;

// Standard base64 with padding; `out` holds 4 * ceil(n / 3) characters.
std::size_t base64(std::span<const std::uint8_t> in, char* out) noexcept;

// The auth token for the private channels and the authenticated REST endpoints:
// "<deadline s>:<account>:<api key>:<hex of the signature over its hash>".
[[nodiscard]] std::string auth_token(const Signer& signer,
                                     std::int64_t deadline_unix_s,
                                     std::int64_t account_index,
                                     std::uint8_t api_key_index);

}  // namespace fastmm::venues::lighter
