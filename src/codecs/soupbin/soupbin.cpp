// SoupBinTCP logical packet builders and parsers (see soupbin.hpp).
#include "fastmm/codecs/soupbin/soupbin.hpp"

#include <cstring>

namespace fastmm::codecs::soupbin {

namespace {
[[nodiscard]] const char* chars(std::span<const std::byte> s) noexcept {
  return reinterpret_cast<const char*>(s.data());
}
}  // namespace

bool payload_size_valid(char type, std::size_t payload) noexcept {
  switch (type) {
    case '+':
    case 'S':
    case 'U':
      return payload <= kMaxPayload;
    case 'A':
      return payload == kLoginAcceptedPayload;
    case 'J':
      return payload == kLoginRejectedPayload;
    case 'L':
      return payload == kLoginRequestPayload || payload == kLoginRequest41Payload;
    case 'H':
    case 'Z':
    case 'R':
    case 'O':
      return payload == 0;
    default:
      return false;
  }
}

std::size_t write_header(std::span<std::byte> out, PacketType type, std::size_t payload) noexcept {
  if (payload > kMaxPayload || out.size() < 3) return 0;
  nasdaq::store_be16(out.data(), static_cast<std::uint16_t>(payload + 1));
  out[2] = static_cast<std::byte>(type);
  return 3;
}

std::size_t write_packet(std::span<std::byte> out,
                         PacketType type,
                         std::span<const std::byte> payload) noexcept {
  if (out.size() < 3 + payload.size() || write_header(out, type, payload.size()) == 0) return 0;
  if (!payload.empty()) std::memcpy(out.data() + 3, payload.data(), payload.size());
  return 3 + payload.size();
}

std::size_t write_login_request(std::span<std::byte> out,
                                std::string_view username,
                                std::string_view password,
                                std::string_view session,
                                std::uint64_t sequence,
                                Version version,
                                std::uint32_t heartbeat_timeout_ms) noexcept {
  if (username.size() > kUsernameLength || password.size() > kPasswordLength ||
      session.size() > kSessionLength)
    return 0;
  const bool v41 = version == Version::V410;
  const std::size_t payload = v41 ? kLoginRequest41Payload : kLoginRequestPayload;
  if (out.size() < 3 + payload) return 0;
  write_header(out, PacketType::LoginRequest, payload);
  auto* p = reinterpret_cast<char*>(out.data() + 3);
  nasdaq::put_alpha(p, kUsernameLength, username);
  nasdaq::put_alpha(p + 6, kPasswordLength, password);
  nasdaq::put_alpha(p + 16, kSessionLength, session);
  if (!nasdaq::put_numeric(p + 26, kSequenceLength, sequence)) return 0;
  if (v41 && !nasdaq::put_numeric(p + 46, kHeartbeatTimeoutLength, heartbeat_timeout_ms)) return 0;
  return 3 + payload;
}

std::size_t write_login_accepted(std::span<std::byte> out,
                                 std::string_view session,
                                 std::uint64_t next_sequence) noexcept {
  if (session.size() > kSessionLength || out.size() < 3 + kLoginAcceptedPayload) return 0;
  write_header(out, PacketType::LoginAccepted, kLoginAcceptedPayload);
  auto* p = reinterpret_cast<char*>(out.data() + 3);
  // "Left padded with spaces" (2.2.1) for both fields.
  std::memset(p, ' ', kSessionLength);
  std::memcpy(p + (kSessionLength - session.size()), session.data(), session.size());
  if (!nasdaq::put_numeric(p + 10, kSequenceLength, next_sequence)) return 0;
  return 3 + kLoginAcceptedPayload;
}

std::size_t write_login_rejected(std::span<std::byte> out, char reason) noexcept {
  if (out.size() < 4) return 0;
  write_header(out, PacketType::LoginRejected, 1);
  out[3] = static_cast<std::byte>(reason);
  return 4;
}

std::size_t write_control(std::span<std::byte> out, PacketType type) noexcept {
  return write_header(out, type, 0);
}

std::size_t write_debug(std::span<std::byte> out, std::string_view text) noexcept {
  return write_packet(out, PacketType::Debug, std::as_bytes(std::span<const char>(text)));
}

bool parse_login_request(std::span<const std::byte> payload, LoginRequestView& out) noexcept {
  if (payload.size() != kLoginRequestPayload && payload.size() != kLoginRequest41Payload)
    return false;
  const char* p = chars(payload);
  out.username = nasdaq::get_alpha_trimmed(p, kUsernameLength);
  out.password = nasdaq::get_alpha_trimmed(p + 6, kPasswordLength);
  out.session = nasdaq::get_alpha_trimmed(p + 16, kSessionLength);
  if (!nasdaq::get_numeric(p + 26, kSequenceLength, out.sequence)) return false;
  out.heartbeat_timeout_ms = 0;
  if (payload.size() == kLoginRequest41Payload) {
    std::uint64_t ms = 0;
    if (!nasdaq::get_numeric(p + 46, kHeartbeatTimeoutLength, ms)) return false;
    out.heartbeat_timeout_ms = static_cast<std::uint32_t>(ms);
  }
  return true;
}

bool parse_login_accepted(std::span<const std::byte> payload, LoginAcceptedView& out) noexcept {
  if (payload.size() != kLoginAcceptedPayload) return false;
  const char* p = chars(payload);
  out.session = nasdaq::get_alpha_trimmed(p, kSessionLength);
  return nasdaq::get_numeric(p + 10, kSequenceLength, out.sequence);
}

bool equal_ignore_case(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 'a' + 'A');
    if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 'a' + 'A');
    if (x != y) return false;
  }
  return true;
}

}  // namespace fastmm::codecs::soupbin
