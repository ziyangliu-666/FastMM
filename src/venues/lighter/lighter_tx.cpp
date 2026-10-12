#include "fastmm/venues/lighter/lighter_tx.hpp"

#include "fastmm/venues/json_writer.hpp"

#include <fmt/format.h>

#include <algorithm>

namespace fastmm::venues::lighter {

namespace {

using u64 = std::uint64_t;

// The value a field contributes: Go's GoldilocksField(int64) keeps the two's complement bits.
constexpr u64 elem(std::int64_t v) noexcept {
  return static_cast<u64>(v);
}

// The attributes' types in ascending order, padded with zeroes to four.
std::array<TxAttributes::Attr, 4> normalized(const TxAttributes& a) noexcept {
  std::array<TxAttributes::Attr, 4> out{};
  for (std::size_t i = 0; i < a.count && i < out.size(); ++i) {
    std::size_t k = i;
    while (k > 0 && out[k - 1].type > a.items[i].type) {
      out[k] = out[k - 1];
      --k;
    }
    out[k] = a.items[i];
  }
  return out;
}

Fp5Bytes aggregate(const Fp5Bytes& tx, const TxAttributes& attrs) noexcept {
  if (attrs.empty()) return tx;
  u64 pairs[8] = {};
  const auto sorted = normalized(attrs);
  for (std::size_t i = 0; i < sorted.size(); ++i) {
    pairs[2 * i] = sorted[i].type;
    pairs[2 * i + 1] = sorted[i].type != 0 ? elem(sorted[i].value) : 0;
  }
  const Fp5Bytes attr = hash_to_quintic(pairs);
  u64 both[10] = {};
  for (std::size_t i = 0; i < 5; ++i) {
    u64 a = 0;
    u64 b = 0;
    for (std::size_t k = 0; k < 8; ++k) {
      a |= static_cast<u64>(tx[i * 8 + k]) << (8 * k);
      b |= static_cast<u64>(attr[i * 8 + k]) << (8 * k);
    }
    both[i] = a;
    both[5 + i] = b;
  }
  return hash_to_quintic(both);
}

template <std::size_t N>
Fp5Bytes finish(const std::array<u64, N>& elems, const TxHeader& h) noexcept {
  return aggregate(hash_to_quintic(elems), h.attributes);
}

void header_elems(u64* e, std::uint32_t chain, TxType type, const TxHeader& h) noexcept {
  e[0] = chain;
  e[1] = type;
  e[2] = elem(h.nonce);
  e[3] = elem(h.expired_at);
  e[4] = elem(h.account_index);
  e[5] = h.api_key_index;
}

// ..,"ExpiredAt":..,"Nonce":..,"Sig":"..","L2TxAttributes":..}
void write_tail(JsonWriter& w, const TxHeader& h, const SigBytes& sig) noexcept {
  w.key("ExpiredAt").integer(h.expired_at);
  w.key("Nonce").integer(h.nonce);
  char b64[112];
  const std::size_t n = base64(sig, b64);
  w.key("Sig").string(std::string_view(b64, n));
  w.key("L2TxAttributes");
  if (h.attributes.empty()) {
    w.null();
  } else {
    // Go writes a map's keys sorted as strings; the types are single digits.
    const auto sorted = normalized(h.attributes);
    w.begin_object();
    for (std::size_t i = 0; i < h.attributes.count; ++i) {
      char key[4];
      const auto r = fmt::format_to_n(key, sizeof key, "{}", sorted[i].type);
      w.key(std::string_view(key, r.size)).integer(sorted[i].value);
    }
    w.end_object();
  }
  w.end_object();
}

std::size_t done(const JsonWriter& w) noexcept {
  return w.ok() ? w.size() : 0;
}

}  // namespace

bool TxAttributes::add(std::uint8_t type, std::int64_t value) noexcept {
  if (count >= items.size()) return false;
  items[count++] = Attr{type, value};
  return true;
}

Fp5Bytes tx_hash(const CreateOrderTx& tx, std::uint32_t chain_id) noexcept {
  std::array<u64, 16> e{};
  header_elems(e.data(), chain_id, kTxCreateOrder, tx.h);
  e[6] = elem(tx.market_index);
  e[7] = elem(tx.client_order_index);
  e[8] = elem(tx.base_amount);
  e[9] = tx.price;
  e[10] = tx.is_ask;
  e[11] = tx.type;
  e[12] = tx.time_in_force;
  e[13] = tx.reduce_only;
  e[14] = tx.trigger_price;
  e[15] = elem(tx.order_expiry);
  return finish(e, tx.h);
}

Fp5Bytes tx_hash(const CancelOrderTx& tx, std::uint32_t chain_id) noexcept {
  std::array<u64, 8> e{};
  header_elems(e.data(), chain_id, kTxCancelOrder, tx.h);
  e[6] = elem(tx.market_index);
  e[7] = elem(tx.index);
  return finish(e, tx.h);
}

Fp5Bytes tx_hash(const ModifyOrderTx& tx, std::uint32_t chain_id) noexcept {
  std::array<u64, 11> e{};
  header_elems(e.data(), chain_id, kTxModifyOrder, tx.h);
  e[6] = elem(tx.market_index);
  e[7] = elem(tx.index);
  e[8] = elem(tx.base_amount);
  e[9] = tx.price;
  e[10] = tx.trigger_price;
  return finish(e, tx.h);
}

Fp5Bytes tx_hash(const CancelAllTx& tx, std::uint32_t chain_id) noexcept {
  std::array<u64, 8> e{};
  header_elems(e.data(), chain_id, kTxCancelAll, tx.h);
  e[6] = tx.time_in_force;
  e[7] = elem(tx.time);
  return finish(e, tx.h);
}

Fp5Bytes tx_hash(const ChangePubKeyTx& tx, std::uint32_t chain_id) noexcept {
  std::array<u64, 11> e{};
  header_elems(e.data(), chain_id, kTxChangePubKey, tx.h);
  for (std::size_t i = 0; i < 5; ++i) {
    u64 v = 0;
    for (std::size_t k = 0; k < 8; ++k) v |= static_cast<u64>(tx.pub_key[i * 8 + k]) << (8 * k);
    e[6 + i] = v;
  }
  return finish(e, tx.h);
}

std::string l1_body(const ChangePubKeyTx& tx) {
  return fmt::format(
      "Register Lighter Account\n\npubkey: 0x{}\nnonce: 0x{:016x}\naccount index: "
      "0x{:016x}\napi key index: 0x{:016x}\nOnly sign this message for a trusted client!",
      to_hex(tx.pub_key),
      static_cast<u64>(tx.h.nonce),
      static_cast<u64>(tx.h.account_index),
      static_cast<u64>(tx.h.api_key_index));
}

std::size_t write_tx_info(const CreateOrderTx& tx,
                          const SigBytes& sig,
                          std::span<char> out) noexcept {
  JsonWriter w(out);
  w.begin_object();
  w.key("AccountIndex").integer(tx.h.account_index);
  w.key("ApiKeyIndex").integer(tx.h.api_key_index);
  w.key("MarketIndex").integer(tx.market_index);
  w.key("ClientOrderIndex").integer(tx.client_order_index);
  w.key("BaseAmount").integer(tx.base_amount);
  w.key("Price").integer(tx.price);
  w.key("IsAsk").integer(tx.is_ask);
  w.key("Type").integer(tx.type);
  w.key("TimeInForce").integer(tx.time_in_force);
  w.key("ReduceOnly").integer(tx.reduce_only);
  w.key("TriggerPrice").integer(tx.trigger_price);
  w.key("OrderExpiry").integer(tx.order_expiry);
  write_tail(w, tx.h, sig);
  return done(w);
}

std::size_t write_tx_info(const CancelOrderTx& tx,
                          const SigBytes& sig,
                          std::span<char> out) noexcept {
  JsonWriter w(out);
  w.begin_object();
  w.key("AccountIndex").integer(tx.h.account_index);
  w.key("ApiKeyIndex").integer(tx.h.api_key_index);
  w.key("MarketIndex").integer(tx.market_index);
  w.key("Index").integer(tx.index);
  write_tail(w, tx.h, sig);
  return done(w);
}

std::size_t write_tx_info(const ModifyOrderTx& tx,
                          const SigBytes& sig,
                          std::span<char> out) noexcept {
  JsonWriter w(out);
  w.begin_object();
  w.key("AccountIndex").integer(tx.h.account_index);
  w.key("ApiKeyIndex").integer(tx.h.api_key_index);
  w.key("MarketIndex").integer(tx.market_index);
  w.key("Index").integer(tx.index);
  w.key("BaseAmount").integer(tx.base_amount);
  w.key("Price").integer(tx.price);
  w.key("TriggerPrice").integer(tx.trigger_price);
  write_tail(w, tx.h, sig);
  return done(w);
}

std::size_t write_tx_info(const CancelAllTx& tx,
                          const SigBytes& sig,
                          std::span<char> out) noexcept {
  JsonWriter w(out);
  w.begin_object();
  w.key("AccountIndex").integer(tx.h.account_index);
  w.key("ApiKeyIndex").integer(tx.h.api_key_index);
  w.key("TimeInForce").integer(tx.time_in_force);
  w.key("Time").integer(tx.time);
  write_tail(w, tx.h, sig);
  return done(w);
}

std::size_t write_tx_info(const ChangePubKeyTx& tx,
                          const SigBytes& sig,
                          std::span<char> out) noexcept {
  JsonWriter w(out);
  w.begin_object();
  w.key("AccountIndex").integer(tx.h.account_index);
  w.key("ApiKeyIndex").integer(tx.h.api_key_index);
  char b64[60];
  const std::size_t n = base64(tx.pub_key, b64);
  w.key("PubKey").string(std::string_view(b64, n));
  w.key("L1Sig").string(tx.l1_sig);
  write_tail(w, tx.h, sig);
  return done(w);
}

std::size_t base64(std::span<const std::uint8_t> in, char* out) noexcept {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::size_t o = 0;
  std::size_t i = 0;
  for (; i + 3 <= in.size(); i += 3) {
    const std::uint32_t v =
        (std::uint32_t{in[i]} << 16) | (std::uint32_t{in[i + 1]} << 8) | std::uint32_t{in[i + 2]};
    out[o++] = kAlphabet[(v >> 18) & 63];
    out[o++] = kAlphabet[(v >> 12) & 63];
    out[o++] = kAlphabet[(v >> 6) & 63];
    out[o++] = kAlphabet[v & 63];
  }
  const std::size_t rest = in.size() - i;
  if (rest > 0) {
    std::uint32_t v = std::uint32_t{in[i]} << 16;
    if (rest == 2) v |= std::uint32_t{in[i + 1]} << 8;
    out[o++] = kAlphabet[(v >> 18) & 63];
    out[o++] = kAlphabet[(v >> 12) & 63];
    out[o++] = rest == 2 ? kAlphabet[(v >> 6) & 63] : '=';
    out[o++] = '=';
  }
  return o;
}

std::string auth_token(const Signer& signer,
                       std::int64_t deadline_unix_s,
                       std::int64_t account_index,
                       std::uint8_t api_key_index) {
  std::string msg = fmt::format("{}:{}:{}", deadline_unix_s, account_index, api_key_index);
  SigBytes sig{};
  if (!signer.sign(hash_bytes_to_quintic(msg), sig)) return {};
  msg += ':';
  msg += to_hex(sig);
  return msg;
}

}  // namespace fastmm::venues::lighter
