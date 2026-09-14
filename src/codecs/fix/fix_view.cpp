#include "fastmm/codecs/fix/fix_view.hpp"

#include <cstring>

namespace fastmm::codecs::fix {

namespace {

// Smallest message: "8=X|9=5|35=0|10=000|" is 20 bytes.
constexpr std::size_t kMinMessage = 20;
constexpr std::size_t kMaxBodyLengthDigits = 7;

// data fields whose value may contain SOH, keyed by the tag of their length field (FIX 4.4:
// SecureDataLen(90)/SecureData(91), RawDataLength(95)/RawData(96), XmlDataLen(212)/XmlData(213),
// EncodedTextLen(354)/EncodedText(355)).
constexpr std::uint32_t data_tag_for_length(std::uint32_t len_tag) noexcept {
  switch (len_tag) {
    case 90:
      return 91;
    case 95:
      return 96;
    case 212:
      return 213;
    case 354:
      return 355;
    default:
      return 0;
  }
}

constexpr bool is_digit(char c) noexcept {
  return c >= '0' && c <= '9';
}

}  // namespace

FixError FixView::parse(std::string_view m,
                        std::string_view begin_string,
                        bool verify_checksum) noexcept {
  clear_index();
  valid_ = false;
  msg_ = m;
  msg_type_ = {};
  const char* p = m.data();
  const std::size_t n = m.size();
  if (n < kMinMessage) return FixError::Truncated;

  // BeginString(8): first field.
  if (p[0] != '8' || p[1] != '=') return FixError::BadBeginString;
  const auto* bs_end = static_cast<const char*>(std::memchr(p + 2, kSoh, n - 2));
  if (bs_end == nullptr || bs_end == p + 2) return FixError::BadBeginString;
  const auto bs_len = static_cast<std::size_t>(bs_end - (p + 2));
  if (!begin_string.empty() && std::string_view(p + 2, bs_len) != begin_string)
    return FixError::BadBeginString;
  add(tag::kBeginString, 2, bs_len);

  // BodyLength(9): second field.
  std::size_t i = static_cast<std::size_t>(bs_end - p) + 1;
  if (i + 2 >= n || p[i] != '9' || p[i + 1] != '=') return FixError::BadBodyLength;
  std::size_t j = i + 2;
  std::size_t body_len = 0;
  while (j < n && is_digit(p[j]) && j - (i + 2) < kMaxBodyLengthDigits + 1) {
    body_len = body_len * 10 + static_cast<std::size_t>(p[j] - '0');
    ++j;
  }
  const std::size_t digits = j - (i + 2);
  if (digits == 0 || digits > kMaxBodyLengthDigits || j >= n || p[j] != kSoh)
    return FixError::BadBodyLength;
  add(tag::kBodyLength, i + 2, digits);
  body_off_ = j + 1;
  trailer_off_ = body_off_ + body_len;
  // Trailer "10=ddd|" must start exactly at body_off_ + BodyLength and end the message.
  if (body_len == 0 || trailer_off_ + 7 != n || p[trailer_off_ - 1] != kSoh ||
      p[trailer_off_] != '1' || p[trailer_off_ + 1] != '0' || p[trailer_off_ + 2] != '=' ||
      p[n - 1] != kSoh)
    return FixError::BadBodyLength;
  if (!is_digit(p[trailer_off_ + 3]) || !is_digit(p[trailer_off_ + 4]) ||
      !is_digit(p[trailer_off_ + 5]))
    return FixError::BadCheckSum;
  const unsigned declared_cks = static_cast<unsigned>(p[trailer_off_ + 3] - '0') * 100U +
                                static_cast<unsigned>(p[trailer_off_ + 4] - '0') * 10U +
                                static_cast<unsigned>(p[trailer_off_ + 5] - '0');

  // Body fields.
  std::size_t k = body_off_;
  std::uint32_t prev_tag = 0;
  std::int64_t prev_value = -1;
  while (k < trailer_off_) {
    const std::size_t t0 = k;
    std::uint32_t t = 0;
    while (k < trailer_off_ && is_digit(p[k])) {
      if (k - t0 >= 9) return FixError::BadField;
      t = t * 10 + static_cast<std::uint32_t>(p[k] - '0');
      ++k;
    }
    if (k == t0 || k >= trailer_off_ || p[k] != '=' || t == 0 || p[t0] == '0')
      return FixError::BadField;
    ++k;
    std::size_t vlen = 0;
    if (prev_tag != 0 && data_tag_for_length(prev_tag) == t && prev_value >= 0) {
      vlen = static_cast<std::size_t>(prev_value);
      if (k + vlen >= trailer_off_ || p[k + vlen] != kSoh) return FixError::BadField;
    } else {
      const auto* e = static_cast<const char*>(std::memchr(p + k, kSoh, trailer_off_ - k));
      if (e == nullptr) return FixError::BadField;
      vlen = static_cast<std::size_t>(e - (p + k));
    }
    if (vlen == 0) return FixError::BadField;
    if (count_ + 1 >= kMaxFields) return FixError::TooManyFields;  // keep a slot for CheckSum
    add(t, k, vlen);
    prev_tag = t;
    prev_value = data_tag_for_length(t) != 0
                     ? parse_fix_int(std::string_view(p + k, vlen)).value_or(-1)
                     : -1;
    k += vlen + 1;
  }
  if (count_ < 3 || entries_[2].tag != tag::kMsgType) return FixError::BadMsgType;
  msg_type_ = std::string_view(p + entries_[2].off, entries_[2].len);
  add(tag::kCheckSum, trailer_off_ + 3, 3);

  if (verify_checksum && checksum(std::string_view(p, trailer_off_)) != declared_cks)
    return FixError::BadCheckSum;
  valid_ = true;
  return FixError::None;
}

}  // namespace fastmm::codecs::fix
