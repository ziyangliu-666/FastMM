#pragma once
// FixBuilder: writes one FIX message into a caller-owned buffer, then backfills BodyLength(9)
// and appends CheckSum(10). No allocation, no exceptions; a buffer overflow latches !ok() and
// finish() returns 0.
//
//   FixBuilder b(buf);
//   b.begin_header("D", "CLIENT", "VENUE", seq, now_ns)       // 8, 9, 35, 49, 56, 34, 52
//    .field(tag::kClOrdID, "fm000100000001")
//    .field_decimal(tag::kPrice, px);
//   std::size_t len = b.finish();                             // 9 and 10 filled in
//
// BodyLength is written with three placeholder digits; finish() shifts the body when the real
// length has a different digit count (bodies under 100 or over 999 bytes). Decimals use the
// exact fixed-point formatter (no double, trailing zeros trimmed: "70000.5", "100"). Standard
// header field order follows the FIX 4.4 StandardHeader component: 8, 9, 35 first, then 49,
// 56, 34, 43, 52, 122 (https://www.onixs.biz/fix-dictionary/4.4/compBlock_StandardHeader.html).
#include "fastmm/codecs/fix/fix_tags.hpp"
#include "fastmm/codecs/fix/fix_time.hpp"
#include "fastmm/core/fixed_point.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace fastmm::codecs::fix {

class FixBuilder {
 public:
  FixBuilder() noexcept = default;
  explicit FixBuilder(std::span<char> buf) noexcept : buf_(buf.data()), cap_(buf.size()) {}
  explicit FixBuilder(std::span<std::byte> buf) noexcept
      : buf_(reinterpret_cast<char*>(buf.data())), cap_(buf.size()) {}

  void reset(std::span<char> buf) noexcept {
    buf_ = buf.data();
    cap_ = buf.size();
    pos_ = 0;
    ok_ = true;
  }

  // "8=<begin_string>|9=000|35=<msg_type>|"
  FixBuilder& begin(std::string_view msg_type,
                    std::string_view begin_string = kBeginString44) noexcept {
    pos_ = 0;
    ok_ = buf_ != nullptr;
    put("8=", 2);
    put(begin_string.data(), begin_string.size());
    put("\x01"
        "9=",
        3);
    len_pos_ = pos_;
    put("000\x01", 4);
    body_start_ = pos_;
    field(tag::kMsgType, msg_type);
    header_end_ = pos_;
    return *this;
  }

  // begin() plus SenderCompID(49), TargetCompID(56), MsgSeqNum(34), PossDupFlag(43) when
  // poss_dup, SendingTime(52) and OrigSendingTime(122) when poss_dup.
  FixBuilder& begin_header(std::string_view msg_type,
                           std::string_view sender,
                           std::string_view target,
                           std::uint64_t seq,
                           std::int64_t sending_ns,
                           bool poss_dup = false,
                           std::int64_t orig_sending_ns = 0,
                           std::string_view begin_string = kBeginString44) noexcept {
    begin(msg_type, begin_string);
    field(tag::kSenderCompID, sender);
    field(tag::kTargetCompID, target);
    field_uint(tag::kMsgSeqNum, seq);
    if (poss_dup) field_char(tag::kPossDupFlag, 'Y');
    field_timestamp(tag::kSendingTime, sending_ns);
    if (poss_dup) field_timestamp(tag::kOrigSendingTime, orig_sending_ns);
    header_end_ = pos_;
    return *this;
  }

  FixBuilder& field(std::uint32_t tag, std::string_view v) noexcept {
    put_tag(tag);
    put(v.data(), v.size());
    put_soh();
    return *this;
  }
  FixBuilder& field_char(std::uint32_t tag, char c) noexcept {
    put_tag(tag);
    put(&c, 1);
    put_soh();
    return *this;
  }
  FixBuilder& field_bool(std::uint32_t tag, bool v) noexcept {
    return field_char(tag, v ? 'Y' : 'N');
  }
  FixBuilder& field_uint(std::uint32_t tag, std::uint64_t v) noexcept {
    put_tag(tag);
    put_uint(v);
    put_soh();
    return *this;
  }
  FixBuilder& field_int(std::uint32_t tag, std::int64_t v) noexcept {
    put_tag(tag);
    if (v < 0) {
      put("-", 1);
      put_uint(static_cast<std::uint64_t>(0) - static_cast<std::uint64_t>(v));
    } else {
      put_uint(static_cast<std::uint64_t>(v));
    }
    put_soh();
    return *this;
  }
  template <class Tag>
  FixBuilder& field_decimal(std::uint32_t tag, Fixed<Tag> v) noexcept {
    put_tag(tag);
    if (reserve(kMaxDecimalChars)) pos_ += v.to_decimal(buf_ + pos_);
    put_soh();
    return *this;
  }
  FixBuilder& field_timestamp(std::uint32_t tag, std::int64_t ns) noexcept {
    put_tag(tag);
    if (reserve(kUtcTimestampMillisChars)) pos_ += format_utc_timestamp(ns, buf_ + pos_);
    put_soh();
    return *this;
  }
  // Pre-formatted "tag=value|..." bytes (e.g. a stored message body on resend).
  FixBuilder& raw(std::string_view bytes) noexcept {
    put(bytes.data(), bytes.size());
    return *this;
  }

  // Backfills BodyLength, appends CheckSum. Returns the total message length, 0 on overflow.
  std::size_t finish() noexcept {
    if (!ok_) return 0;
    const std::size_t body_len = pos_ - body_start_;
    const std::size_t digits = count_digits(body_len);
    if (digits != 3) {
      if (digits > 3 && !reserve(digits - 3 + 7)) return 0;
      const std::size_t new_start = len_pos_ + digits + 1;
      std::memmove(buf_ + new_start, buf_ + body_start_, body_len);
      header_end_ = header_end_ - body_start_ + new_start;
      body_start_ = new_start;
      pos_ = new_start + body_len;
    }
    write_uint_at(len_pos_, body_len, digits);
    buf_[len_pos_ + digits] = kSoh;
    if (!reserve(7)) return 0;
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < pos_; ++i) sum += static_cast<unsigned char>(buf_[i]);
    const std::uint32_t cks = sum & 0xFFU;
    buf_[pos_] = '1';
    buf_[pos_ + 1] = '0';
    buf_[pos_ + 2] = '=';
    buf_[pos_ + 3] = static_cast<char>('0' + cks / 100);
    buf_[pos_ + 4] = static_cast<char>('0' + (cks / 10) % 10);
    buf_[pos_ + 5] = static_cast<char>('0' + cks % 10);
    buf_[pos_ + 6] = kSoh;
    pos_ += 7;
    return pos_;
  }

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] std::size_t size() const noexcept { return pos_; }
  [[nodiscard]] std::string_view view() const noexcept { return {buf_, pos_}; }
  // Offset of the first byte after the standard header written by begin()/begin_header().
  [[nodiscard]] std::size_t header_end() const noexcept { return header_end_; }

 private:
  static constexpr std::size_t count_digits(std::uint64_t v) noexcept {
    std::size_t n = 1;
    while (v >= 10) {
      v /= 10;
      ++n;
    }
    return n;
  }
  bool reserve(std::size_t n) noexcept {
    if (!ok_ || pos_ + n > cap_) {
      ok_ = false;
      return false;
    }
    return true;
  }
  void put(const char* s, std::size_t n) noexcept {
    if (!reserve(n)) return;
    std::memcpy(buf_ + pos_, s, n);
    pos_ += n;
  }
  void put_soh() noexcept {
    if (!reserve(1)) return;
    buf_[pos_++] = kSoh;
  }
  void write_uint_at(std::size_t at, std::uint64_t v, std::size_t digits) noexcept {
    for (std::size_t i = digits; i > 0; --i) {
      buf_[at + i - 1] = static_cast<char>('0' + v % 10);
      v /= 10;
    }
  }
  void put_uint(std::uint64_t v) noexcept {
    const std::size_t d = count_digits(v);
    if (!reserve(d)) return;
    write_uint_at(pos_, v, d);
    pos_ += d;
  }
  void put_tag(std::uint32_t tag) noexcept {
    put_uint(tag);
    if (!reserve(1)) return;
    buf_[pos_++] = '=';
  }

  char* buf_ = nullptr;
  std::size_t cap_ = 0;
  std::size_t pos_ = 0;
  std::size_t len_pos_ = 0;
  std::size_t body_start_ = 0;
  std::size_t header_end_ = 0;
  bool ok_ = false;
};

}  // namespace fastmm::codecs::fix
