#pragma once
// SoupBinTCP logical packets (fastmm::codecs::soupbin).
//
// Sources (Nasdaq): "SoupBinTCP Version 3.00" (soupbintcp.pdf), "SoupBinTCP Version 4.00"
// (SoupBinTCP 4.0.pdf, 07/12/2010: adds Unsequenced Data packets from the server) and
// "SoupBinTCP Version 4.10" (SoupBinTCP41.pdf, 01/13/2012: adds the 5-byte Heartbeat Timeout
// to the Login Request). Every logical packet is
//
//   Packet Length(2, big-endian, = 1 + payload) | Packet Type(1) | payload
//
//   '+' Debug             either side   free text, ignored
//   'A' Login Accepted    server        Session(10, alnum) | Sequence Number(20, ASCII)
//   'J' Login Rejected    server        Reject Reason Code(1): 'A' not authorized,
//                                       'S' session not available
//   'S' Sequenced Data    server        one higher-level message, sequence number implied
//   'U' Unsequenced Data  client (all versions), server (4.00+)
//   'H' Server Heartbeat  server
//   'Z' End of Session    server
//   'L' Login Request     client        Username(6) | Password(10) | Requested Session(10) |
//                                       Requested Sequence Number(20, ASCII)
//                                       [| Heartbeat Timeout(5, ASCII ms), 4.10 only]
//   'R' Client Heartbeat  client
//   'O' Logout Request    client
//
// ASCII numeric fields are written right-aligned, left padded with spaces (the Login Accepted
// description says so; the Login Request does not say, so parsing accepts either padding).
#include "fastmm/codecs/codec.hpp"
#include "fastmm/codecs/itch/nasdaq_fields.hpp"
#include "fastmm/core/config_macros.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace fastmm::codecs::soupbin {

enum class PacketType : char {
  Debug = '+',
  LoginAccepted = 'A',
  LoginRejected = 'J',
  SequencedData = 'S',
  UnsequencedData = 'U',
  ServerHeartbeat = 'H',
  EndOfSession = 'Z',
  LoginRequest = 'L',
  ClientHeartbeat = 'R',
  LogoutRequest = 'O',
};

enum class Version : std::uint8_t { V300 = 0, V400 = 1, V410 = 2 };

inline constexpr char kRejectNotAuthorized = 'A';
inline constexpr char kRejectSessionNotAvailable = 'S';

inline constexpr std::size_t kSessionLength = 10;
inline constexpr std::size_t kSequenceLength = 20;
inline constexpr std::size_t kUsernameLength = 6;
inline constexpr std::size_t kPasswordLength = 10;
inline constexpr std::size_t kHeartbeatTimeoutLength = 5;
inline constexpr std::size_t kMaxPayload = 0xFFFF - 1;  // Packet Length counts the type byte

#pragma pack(push, 1)
struct PacketHeader {
  be16_t packet_length;
  char packet_type;
};
struct LoginAcceptedPacket {
  PacketHeader hdr;
  char session[10];
  char sequence_number[20];
};
struct LoginRejectedPacket {
  PacketHeader hdr;
  char reject_reason_code;
};
struct LoginRequestPacket {
  PacketHeader hdr;
  char username[6];
  char password[10];
  char requested_session[10];
  char requested_sequence_number[20];
};
struct LoginRequestPacket41 {
  PacketHeader hdr;
  char username[6];
  char password[10];
  char requested_session[10];
  char requested_sequence_number[20];
  char heartbeat_timeout[5];
};
#pragma pack(pop)
static_assert(sizeof(PacketHeader) == 3);
static_assert(sizeof(LoginAcceptedPacket) == 33 && offsetof(LoginAcceptedPacket, session) == 3 &&
              offsetof(LoginAcceptedPacket, sequence_number) == 13);
static_assert(sizeof(LoginRejectedPacket) == 4);
static_assert(sizeof(LoginRequestPacket) == 49 && offsetof(LoginRequestPacket, username) == 3 &&
              offsetof(LoginRequestPacket, password) == 9 &&
              offsetof(LoginRequestPacket, requested_session) == 19 &&
              offsetof(LoginRequestPacket, requested_sequence_number) == 29);
static_assert(sizeof(LoginRequestPacket41) == 54 &&
              offsetof(LoginRequestPacket41, heartbeat_timeout) == 49);

// Payload sizes (bytes after the type byte) of the fixed-size packets.
inline constexpr std::size_t kLoginAcceptedPayload = sizeof(LoginAcceptedPacket) - 3;
inline constexpr std::size_t kLoginRejectedPayload = sizeof(LoginRejectedPacket) - 3;
inline constexpr std::size_t kLoginRequestPayload = sizeof(LoginRequestPacket) - 3;
inline constexpr std::size_t kLoginRequest41Payload = sizeof(LoginRequestPacket41) - 3;

// Splits a TCP byte stream into logical packets without copying: payload excludes the type
// byte and kind is the packet type. A zero Packet Length (no type byte) is a protocol
// violation; it is returned as a complete frame with kind 0 so the stream still advances.
struct SoupBinFramer {
  [[nodiscard]] FrameView next(std::span<const std::byte> in) const noexcept {
    if (in.size() < 2) return {};
    const std::size_t len = nasdaq::load_be16(in.data());
    if (FASTMM_UNLIKELY(len == 0)) return FrameView{{}, 2, 0};
    if (in.size() < 2 + len) return {};
    return FrameView{in.subspan(3, len - 1), 2 + len, std::to_integer<std::uint8_t>(in[2])};
  }
};
static_assert(Framer<SoupBinFramer>);

// True when the payload size is legal for the packet type (unknown types: false).
[[nodiscard]] bool payload_size_valid(char type, std::size_t payload) noexcept;

// ---- builders: bytes written, 0 when `out` is too small or a field does not fit ----------

std::size_t write_packet(std::span<std::byte> out,
                         PacketType type,
                         std::span<const std::byte> payload) noexcept;
// Header only (3 bytes) for a packet whose payload the caller sends separately.
std::size_t write_header(std::span<std::byte> out, PacketType type, std::size_t payload) noexcept;
std::size_t write_login_request(std::span<std::byte> out,
                                std::string_view username,
                                std::string_view password,
                                std::string_view session,
                                std::uint64_t sequence,
                                Version version,
                                std::uint32_t heartbeat_timeout_ms) noexcept;
std::size_t write_login_accepted(std::span<std::byte> out,
                                 std::string_view session,
                                 std::uint64_t next_sequence) noexcept;
std::size_t write_login_rejected(std::span<std::byte> out, char reason) noexcept;
// H, Z, R, O (no payload).
std::size_t write_control(std::span<std::byte> out, PacketType type) noexcept;
std::size_t write_debug(std::span<std::byte> out, std::string_view text) noexcept;

// ---- parsers over a frame payload ----------------------------------------------------------

struct LoginRequestView {
  std::string_view username;  // trimmed
  std::string_view password;  // trimmed
  std::string_view session;   // trimmed, empty == current session
  std::uint64_t sequence = 0;
  std::uint32_t heartbeat_timeout_ms = 0;  // 0 when absent (3.00 / 4.00 layout)
};
[[nodiscard]] bool parse_login_request(std::span<const std::byte> payload,
                                       LoginRequestView& out) noexcept;

struct LoginAcceptedView {
  std::string_view session;  // trimmed
  std::uint64_t sequence = 0;
};
[[nodiscard]] bool parse_login_accepted(std::span<const std::byte> payload,
                                        LoginAcceptedView& out) noexcept;

// Case-insensitive comparison of trimmed alphanumeric fields (usernames, passwords).
[[nodiscard]] bool equal_ignore_case(std::string_view a, std::string_view b) noexcept;

}  // namespace fastmm::codecs::soupbin
