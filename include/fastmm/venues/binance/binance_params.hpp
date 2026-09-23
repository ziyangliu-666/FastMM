#pragma once
// The parameters of one Binance request and the two ways a request is finished. Spot and
// USDⓈ-M sign identically (rest-api.md "SIGNED Endpoint security", web-socket-api.md "SIGNED
// request security"): parameters sorted by name, joined as k=v&k=v, HMAC-SHA256 as lowercase
// hex or Ed25519 as base64, sent as `signature`. So this is one implementation; only the
// parameter names and endpoints differ between the two connectors.
#include "fastmm/net/crypto.hpp"
#include "fastmm/net/url.hpp"
#include "fastmm/venues/binance/binance_auth.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/json_writer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace fastmm::venues::binance {

inline constexpr std::size_t kMaxRequestBytes = 1536;

// `N` is the most parameters any of the caller's requests takes. Text values are copied into a
// flat arena so the signature payload and the JSON see identical bytes.
template <std::size_t N>
struct BinanceParams {
  struct Param {
    std::string_view key;
    std::string_view value;
    bool numeric;  // emitted unquoted in JSON (timestamp, recvWindow, orderId)
  };
  std::array<Param, N> items{};
  std::size_t count = 0;
  char arena[512];
  std::size_t used = 0;
  bool ok = true;

  std::string_view intern(std::string_view v) noexcept {
    if (used + v.size() > sizeof arena) {
      ok = false;
      return {};
    }
    std::memcpy(arena + used, v.data(), v.size());
    std::string_view out(arena + used, v.size());
    used += v.size();
    return out;
  }
  void add(std::string_view key, std::string_view value, bool numeric = false) noexcept {
    if (count >= items.size()) {
      ok = false;
      return;
    }
    items[count++] = Param{key, intern(value), numeric};
  }
  void add_int(std::string_view key, std::int64_t v) noexcept {
    char buf[24];
    add(key, std::string_view(buf, format_int64(v, buf)), true);
  }
  template <class Tag>
  void add_decimal(std::string_view key, Fixed<Tag> v) noexcept {
    DecimalText t(v);
    add(key, t.view(), false);
  }
  // Insertion order must already be alphabetical; assert it so a new field cannot silently
  // break the signature.
  [[nodiscard]] bool sorted() const noexcept {
    for (std::size_t i = 1; i < count; ++i) {
      if (!(items[i - 1].key < items[i].key)) return false;
    }
    return true;
  }
  template <std::size_t Q>
  void to_query(net::QueryBuilder<Q>& q) const noexcept {
    for (std::size_t i = 0; i < count; ++i) q.add(items[i].key, items[i].value);
  }
};

// WS API frame: {"id":..,"method":..,"params":{..,"signature":..}}. Returns the bytes written,
// 0 on failure (parameters unsorted or overfull, signing failed, `out` too small). A logged-on
// Ed25519 session (`session_auth`) sends no apiKey and no signature.
template <std::size_t N>
std::size_t write_signed_ws_request(const BinanceParams<N>& params,
                                    const Signer& signer,
                                    bool session_auth,
                                    std::string_view method,
                                    std::string_view request_id,
                                    std::span<char> out) noexcept {
  if (!params.ok || !params.sorted()) return 0;
  std::string_view signature;
  net::HexSha256 hmac;
  char ed[net::kEd25519Base64Size];
  const bool signed_request = !session_auth && signer.usable();
  if (signed_request) {
    // Signature payload: sorted "k=v&k=v" (values percent-encoded exactly as the REST variant;
    // decimals and ids contain only unreserved characters).
    net::QueryBuilder<kMaxRequestBytes> q;
    params.to_query(q);
    if (!q.ok()) return 0;
    if (signer.type() == KeyType::Hmac) {
      hmac = signer.sign_hmac(q.view());
      signature = hmac.view();
    } else {
      // Only before session.logon completes (or for session.logon itself).
      const std::size_t n = signer.sign_ed25519(q.view(), ed);
      if (n == 0) return 0;
      signature = std::string_view(ed, n);
    }
  }
  JsonWriter w(out);
  w.begin_object()
      .key("id")
      .string(request_id)
      .key("method")
      .string(method)
      .key("params")
      .begin_object();
  for (std::size_t i = 0; i < params.count; ++i) {
    const auto& p = params.items[i];
    w.key(p.key);
    if (p.numeric) {
      w.raw_value(p.value);
    } else {
      w.string(p.value);
    }
  }
  if (signed_request) w.key("signature").string(signature);
  w.end_object().end_object();
  return w.ok() ? w.size() : 0;
}

// REST: the sorted query plus `signature` (the key travels in the X-MBX-APIKEY header).
template <std::size_t N, std::size_t Q>
bool write_signed_rest_query(const BinanceParams<N>& params,
                             const Signer& signer,
                             net::QueryBuilder<Q>& query) noexcept {
  if (!params.ok || !params.sorted()) return false;
  query.clear();
  params.to_query(query);
  if (!query.ok()) return false;
  return signer.sign_query(query);
}

}  // namespace fastmm::venues::binance
