#include "fastmm/net/ws_client.hpp"

#include "fastmm/net/crypto.hpp"

#include <cstdio>
#include <cstring>

namespace fastmm::net::detail {

namespace {
constexpr std::string_view kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
}

void ws_compute_accept_key(std::string_view client_key,
                           std::span<char, kWsAcceptLen> out) noexcept {
  char concat[kWsKeyLen + kWsGuid.size() + 8];
  const std::size_t key_len = client_key.size() < kWsKeyLen + 8 ? client_key.size() : kWsKeyLen + 8;
  std::memcpy(concat, client_key.data(), key_len);
  std::memcpy(concat + key_len, kWsGuid.data(), kWsGuid.size());
  const Sha1Digest digest = sha1(std::string_view(concat, key_len + kWsGuid.size()));
  base64_encode(std::span<const std::uint8_t>(digest), std::span<char>(out));
}

void ws_generate_client_key(std::span<char, kWsKeyLen> out) noexcept {
  std::uint8_t nonce[16];
  random_bytes(nonce);
  base64_encode(std::span<const std::uint8_t>(nonce), std::span<char>(out));
}

bool ws_build_upgrade_request(WireBuffer& out,
                              std::string_view host,
                              std::uint16_t port,
                              bool tls,
                              std::string_view target,
                              std::string_view key,
                              std::string_view extra_headers) noexcept {
  char port_buf[8] = {};
  const bool default_port = port == 0 || port == (tls ? 443 : 80);
  if (!default_port) std::snprintf(port_buf, sizeof(port_buf), ":%u", port);
  const std::string_view parts[] = {
      "GET ",
      target,
      " HTTP/1.1\r\nHost: ",
      host,
      port_buf,
      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ",
      key,
      "\r\nSec-WebSocket-Version: 13\r\n",
      extra_headers,
      "\r\n",
  };
  std::size_t need = 0;
  for (std::string_view p : parts) need += p.size();
  if (!out.reserve(need)) return false;
  for (std::string_view p : parts) out.append(p);  // cannot fail after reserve()
  return true;
}

const char* ws_check_upgrade_response(const HttpResponseHead& head,
                                      std::string_view expected_accept) noexcept {
  if (head.status != 101) return "upgrade rejected (non-101 status)";
  if (!iequals(head.headers.get("Upgrade"), "websocket")) return "missing Upgrade: websocket";
  if (!header_has_token(head.headers.get("Connection"), "upgrade"))
    return "missing Connection: Upgrade";
  if (head.headers.get("Sec-WebSocket-Accept") != expected_accept)
    return "Sec-WebSocket-Accept mismatch";
  // We never offer extensions; a server picking one anyway violates §4.2.2 / §9.1.
  if (head.headers.has("Sec-WebSocket-Extensions"))
    return "unsolicited extension (permessage-deflate not supported)";
  return nullptr;
}

MaskGenerator::MaskGenerator() noexcept {
  std::uint8_t seed[8];
  random_bytes(seed);
  std::memcpy(&state_, seed, 8);
  if (state_ == 0) state_ = 0x9E3779B97F4A7C15ULL;  // xorshift must not start at 0
}

}  // namespace fastmm::net::detail
