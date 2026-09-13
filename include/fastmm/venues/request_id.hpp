#pragma once
// Request ids shared by the JSON order-entry encoders: "<kind><cl_ord_id>" with kind n/c/r
// (1 + 14 chars), so a WebSocket API response maps back to the command with no lookup
// table. Binance allows a 36-char string id, Bybit a 36-char reqId.
#include "fastmm/core/fixed_string.hpp"
#include "fastmm/core/strong_id.hpp"

#include <optional>
#include <string_view>
#include <utility>

namespace fastmm::venues {

using RequestId = FixedString<16>;
enum class RequestKind : char { New = 'n', Cancel = 'c', Replace = 'r', Other = 'x' };

[[nodiscard]] inline RequestId make_request_id(RequestKind kind, ClientOrderId id) noexcept {
  RequestId out;
  out.push_back(static_cast<char>(kind));
  out.append(encode_cl_ord_id(id).view());
  return out;
}
// Returns the kind and id encoded by make_request_id, or nullopt for foreign ids.
[[nodiscard]] inline std::optional<std::pair<RequestKind, ClientOrderId>> parse_request_id(
    std::string_view id) noexcept {
  if (id.size() != 1 + kClOrdIdChars) return std::nullopt;
  const char k = id[0];
  if (k != 'n' && k != 'c' && k != 'r') return std::nullopt;
  const auto cl = decode_cl_ord_id(id.substr(1));
  if (!cl) return std::nullopt;
  return std::make_pair(static_cast<RequestKind>(k), *cl);
}

}  // namespace fastmm::venues
