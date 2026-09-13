#pragma once
// RFC 6455 framing: header codec, masking, and the message assembler shared by the client
// and server. No allocation; the assembler works in place inside a RecvBuffer.
//
//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//  +-+-+-+-+-------+-+-------------+-------------------------------+
//  |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
//  |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
//  |N|V|V|V|       |S|             |                               |
//  +-+-+-+-+-------+-+-------------+-------------------------------+
//  |          Masking-key (if MASK set, 4 bytes)                   |
//  +---------------------------------------------------------------+
#include "fastmm/net/recv_buffer.hpp"
#include "fastmm/net/wire_buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace fastmm::net {

enum class WsOpcode : std::uint8_t {
  Continuation = 0x0,
  Text = 0x1,
  Binary = 0x2,
  Close = 0x8,
  Ping = 0x9,
  Pong = 0xA,
};

constexpr bool ws_is_control(WsOpcode op) noexcept {
  return (static_cast<std::uint8_t>(op) & 0x8) != 0;
}
constexpr bool ws_is_known(std::uint8_t op) noexcept {
  return op <= 2 || (op >= 8 && op <= 10);
}

// RFC 6455 §7.4.1 status codes (the ones a client needs to send or interpret).
enum class WsCloseCode : std::uint16_t {
  Normal = 1000,
  GoingAway = 1001,
  ProtocolError = 1002,
  Unsupported = 1003,
  NoStatus = 1005,  // reserved: close frame carried no code
  Abnormal = 1006,  // reserved: connection dropped without a close frame
  InvalidPayload = 1007,
  PolicyViolation = 1008,
  MessageTooBig = 1009,
  MandatoryExtension = 1010,
  InternalError = 1011,
};

inline constexpr std::size_t kWsMaxHeaderSize = 14;  // 2 + 8 (64-bit length) + 4 (mask)
inline constexpr std::size_t kWsMaxControlPayload = 125;

struct WsFrameHeader {
  std::uint64_t payload_len = 0;
  std::uint8_t header_len = 0;
  WsOpcode opcode = WsOpcode::Continuation;
  bool fin = false;
  bool rsv = false;  // any RSV bit set (extensions; permessage-deflate would live here)
  bool masked = false;
  std::uint8_t mask[4] = {0, 0, 0, 0};
};

enum class WsParseStatus : std::uint8_t { Ok, Incomplete, Invalid };

// Parses the header at the start of `in`. Invalid: reserved opcode, or a 16/64-bit length
// that is not minimally encoded / has the top bit set (§5.2).
inline WsParseStatus parse_frame_header(std::span<const std::byte> in, WsFrameHeader& h) noexcept {
  if (in.size() < 2) return WsParseStatus::Incomplete;
  const auto b0 = static_cast<std::uint8_t>(in[0]);
  const auto b1 = static_cast<std::uint8_t>(in[1]);
  h.fin = (b0 & 0x80) != 0;
  h.rsv = (b0 & 0x70) != 0;
  const std::uint8_t op = b0 & 0x0F;
  if (!ws_is_known(op)) return WsParseStatus::Invalid;
  h.opcode = static_cast<WsOpcode>(op);
  h.masked = (b1 & 0x80) != 0;
  const std::uint8_t len7 = b1 & 0x7F;
  std::size_t pos = 2;
  if (len7 < 126) {
    h.payload_len = len7;
  } else if (len7 == 126) {
    if (in.size() < 4) return WsParseStatus::Incomplete;
    h.payload_len = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(in[2])) << 8) |
                    static_cast<std::uint64_t>(static_cast<std::uint8_t>(in[3]));
    if (h.payload_len < 126) return WsParseStatus::Invalid;
    pos = 4;
  } else {
    if (in.size() < 10) return WsParseStatus::Incomplete;
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
      v = (v << 8) | static_cast<std::uint8_t>(in[2 + static_cast<std::size_t>(i)]);
    if (v < 65536 || (v >> 63) != 0) return WsParseStatus::Invalid;
    h.payload_len = v;
    pos = 10;
  }
  if (h.masked) {
    if (in.size() < pos + 4) return WsParseStatus::Incomplete;
    std::memcpy(h.mask, in.data() + pos, 4);
    pos += 4;
  }
  h.header_len = static_cast<std::uint8_t>(pos);
  return WsParseStatus::Ok;
}

constexpr std::size_t ws_header_size(std::uint64_t payload_len, bool masked) noexcept {
  const std::size_t len_bytes = payload_len < 126 ? 2u : payload_len <= 0xFFFF ? 4u : 10u;
  return len_bytes + (masked ? 4u : 0u);
}

// Writes a header for `payload_len` bytes; `mask` may be nullptr (server -> client frames).
// Returns the header length (<= kWsMaxHeaderSize).
inline std::size_t encode_frame_header(std::span<std::byte> out,
                                       WsOpcode opcode,
                                       bool fin,
                                       std::uint64_t payload_len,
                                       const std::uint8_t* mask) noexcept {
  const std::size_t need = ws_header_size(payload_len, mask != nullptr);
  if (out.size() < need) return 0;
  out[0] = static_cast<std::byte>((fin ? 0x80 : 0x00) | static_cast<std::uint8_t>(opcode));
  const std::uint8_t mask_bit = mask != nullptr ? 0x80 : 0x00;
  std::size_t pos = 2;
  if (payload_len < 126) {
    out[1] = static_cast<std::byte>(mask_bit | static_cast<std::uint8_t>(payload_len));
  } else if (payload_len <= 0xFFFF) {
    out[1] = static_cast<std::byte>(mask_bit | 126);
    out[2] = static_cast<std::byte>(payload_len >> 8);
    out[3] = static_cast<std::byte>(payload_len & 0xFF);
    pos = 4;
  } else {
    out[1] = static_cast<std::byte>(mask_bit | 127);
    for (int i = 0; i < 8; ++i) {
      out[2 + static_cast<std::size_t>(i)] = static_cast<std::byte>(payload_len >> (56 - 8 * i));
    }
    pos = 10;
  }
  if (mask != nullptr) {
    std::memcpy(out.data() + pos, mask, 4);
    pos += 4;
  }
  return pos;
}

// XOR (un)masking in place, 8 bytes at a time. `offset` is the position of data[0] within
// the frame payload (for resuming across chunks; 0 for whole payloads).
inline void ws_mask_inplace(std::span<std::byte> data,
                            const std::uint8_t mask[4],
                            std::size_t offset = 0) noexcept {
  const std::uint8_t rot[4] = {
      mask[offset & 3], mask[(offset + 1) & 3], mask[(offset + 2) & 3], mask[(offset + 3) & 3]};
  std::uint64_t m64 = 0;
  const std::uint8_t m8[8] = {rot[0], rot[1], rot[2], rot[3], rot[0], rot[1], rot[2], rot[3]};
  std::memcpy(&m64, m8, 8);
  std::byte* p = data.data();
  std::size_t i = 0;
  for (; i + 8 <= data.size(); i += 8) {
    std::uint64_t v = 0;
    std::memcpy(&v, p + i, 8);
    v ^= m64;
    std::memcpy(p + i, &v, 8);
  }
  for (; i < data.size(); ++i) p[i] ^= static_cast<std::byte>(rot[i & 3]);
}

// Appends a complete frame (header + payload, masked if `mask` != nullptr) to `out`.
// Returns false (nothing written) if the buffer cannot hold it.
inline bool ws_encode_frame(WireBuffer& out,
                            WsOpcode opcode,
                            bool fin,
                            std::span<const std::byte> payload,
                            const std::uint8_t* mask) noexcept {
  const std::size_t hdr = ws_header_size(payload.size(), mask != nullptr);
  if (!out.reserve(hdr + payload.size())) return false;
  const std::size_t payload_offset = out.size();  // relative to pending().data()
  encode_frame_header(
      std::span<std::byte>(out.write_ptr(), hdr), opcode, fin, payload.size(), mask);
  out.commit(hdr);
  out.append(payload);
  if (mask != nullptr) out.mask_xor_inplace(payload_offset + hdr, payload.size(), mask);
  return true;
}

// Close frame payload: 2-byte big-endian code + UTF-8 reason.
inline std::size_t ws_encode_close_payload(std::span<std::byte> out,
                                           std::uint16_t code,
                                           std::string_view reason) noexcept {
  const std::size_t n = 2 + reason.size();
  if (out.size() < n || n > kWsMaxControlPayload) return 0;
  out[0] = static_cast<std::byte>(code >> 8);
  out[1] = static_cast<std::byte>(code & 0xFF);
  if (!reason.empty()) std::memcpy(out.data() + 2, reason.data(), reason.size());
  return n;
}

inline std::uint16_t ws_decode_close_code(std::span<const std::byte> payload) noexcept {
  if (payload.size() < 2) return static_cast<std::uint16_t>(WsCloseCode::NoStatus);
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(payload[0]) << 8) |
                                    static_cast<std::uint16_t>(payload[1]));
}

namespace detail {

struct WsAssembleError {
  WsCloseCode code = WsCloseCode::Normal;  // Normal == no error
  const char* detail = "";
  explicit operator bool() const noexcept { return code != WsCloseCode::Normal; }
};

// Turns the bytes in a RecvBuffer into messages, in place:
//   * an unfragmented frame is delivered as [payload) and consumed;
//   * a fragmented message is compacted: the first fragment's payload is moved to the front
//     of the unread region, every continuation payload is moved directly behind it (its
//     header, and any interleaved control frame, become a dead gap that is consumed with
//     the final fragment). The message is therefore always contiguous, and text payloads
//     keep RecvBuffer::kPadding readable bytes behind them for simdjson.
// The sink receives:
//   on_message(WsOpcode, std::span<std::byte>)  -- Text/Binary, complete
//   on_control(WsOpcode, std::span<std::byte>)  -- Ping/Pong/Close (unmasked already)
// Sink callbacks may send but must not consume from the buffer.
class WsMessageAssembler {
 public:
  WsMessageAssembler(RecvBuffer& rx, std::size_t max_message_bytes, bool require_masked) noexcept
      : rx_(rx), max_message_(max_message_bytes), require_masked_(require_masked) {}

  template <class Sink>
  WsAssembleError process(Sink& sink) noexcept {
    for (;;) {
      std::span<std::byte> all = rx_.readable();
      if (parse_off_ >= all.size()) return {};
      std::span<std::byte> in = all.subspan(parse_off_);
      WsFrameHeader h;
      const WsParseStatus st = parse_frame_header(in, h);
      if (st == WsParseStatus::Incomplete) return {};
      if (st == WsParseStatus::Invalid) return fail(WsCloseCode::ProtocolError, "bad frame header");
      if (h.rsv) return fail(WsCloseCode::ProtocolError, "rsv bits set without extension");
      if (h.masked != require_masked_) {
        return fail(WsCloseCode::ProtocolError,
                    require_masked_ ? "unmasked client frame" : "masked server frame");
      }
      const bool control = ws_is_control(h.opcode);
      if (control && (!h.fin || h.payload_len > kWsMaxControlPayload)) {
        return fail(WsCloseCode::ProtocolError, "fragmented or oversized control frame");
      }
      if (!control && h.payload_len + (frag_active_ ? frag_len_ : 0) > max_message_) {
        return fail(WsCloseCode::MessageTooBig, "message exceeds limit");
      }
      const std::size_t total = h.header_len + static_cast<std::size_t>(h.payload_len);
      if (parse_off_ + total > rx_.capacity())
        return fail(WsCloseCode::MessageTooBig, "frame exceeds buffer");
      if (in.size() < total) return {};  // wait for the rest of the payload

      std::span<std::byte> payload =
          in.subspan(h.header_len, static_cast<std::size_t>(h.payload_len));
      if (h.masked) ws_mask_inplace(payload, h.mask);

      if (control) {
        sink.on_control(h.opcode, payload);
        if (frag_active_) {
          parse_off_ += total;  // leave it in the gap; consumed with the final fragment
        } else {
          rx_.consume(total);
        }
        continue;
      }

      if (h.opcode == WsOpcode::Continuation) {
        if (!frag_active_) return fail(WsCloseCode::ProtocolError, "continuation without start");
        std::byte* base = all.data();
        std::memmove(base + frag_len_, payload.data(), payload.size());
        frag_len_ += payload.size();
        parse_off_ += total;
        if (h.fin) {
          sink.on_message(frag_opcode_, std::span<std::byte>(base, frag_len_));
          rx_.consume(parse_off_);
          reset_fragment();
        }
        continue;
      }

      // Text / Binary
      if (frag_active_)
        return fail(WsCloseCode::ProtocolError, "new data frame inside fragmented message");
      if (h.fin) {
        sink.on_message(h.opcode, payload);
        rx_.consume(total);
        continue;
      }
      std::byte* base = all.data();
      std::memmove(base, payload.data(), payload.size());  // drop the header: message starts at rd_
      frag_active_ = true;
      frag_opcode_ = h.opcode;
      frag_len_ = payload.size();
      parse_off_ = total;
    }
  }

  // True while a fragmented message is being assembled (bytes pinned in the buffer).
  bool fragment_active() const noexcept { return frag_active_; }
  void reset() noexcept { reset_fragment(); }

 private:
  WsAssembleError fail(WsCloseCode code, const char* detail) noexcept {
    return WsAssembleError{code, detail};
  }
  void reset_fragment() noexcept {
    frag_active_ = false;
    frag_len_ = 0;
    parse_off_ = 0;
  }

  RecvBuffer& rx_;
  std::size_t max_message_;
  bool require_masked_;
  bool frag_active_ = false;
  WsOpcode frag_opcode_ = WsOpcode::Text;
  std::size_t frag_len_ = 0;   // assembled message bytes at readable()[0..frag_len_)
  std::size_t parse_off_ = 0;  // where the next frame header starts, relative to readable()
};

}  // namespace detail
}  // namespace fastmm::net
