#pragma once
// FixView: zero-copy, validating index over one FIX tag=value message.
//
//   8=FIX.4.4|9=<BodyLength>|35=<MsgType>|...body...|10=<CheckSum>|      ('|' is SOH, 0x01)
//
// parse() checks the framing rules of the FIX 4.4 session protocol and indexes every field as
// (tag, offset, length) into the caller's bytes; nothing is copied and nothing allocates:
//   * BeginString(8) must be the first field (and equal the expected value),
//   * BodyLength(9) the second, counting the bytes after its SOH up to "10=",
//   * MsgType(35) the third,
//   * CheckSum(10) the last: three digits, the sum of every byte before "10=" modulo 256.
// Sources: OnixS FIX 4.4 dictionary, StandardHeader / BodyLength(9) / CheckSum(10)
// (https://www.onixs.biz/fix-dictionary/4.4/compBlock_StandardHeader.html,
// tagNum_9.html, tagNum_10.html) and the CheckSum calculation appendix
// (https://www.onixs.biz/fix-dictionary/4.2/app_b.html, unchanged in 4.4).
//
// Getters return std::optional and never throw; an absent field and a malformed value are both
// nullopt (FIX forbids empty values, so parse() rejects "tag=" with no value). Values of the
// data-typed fields SecureData(91), RawData(96), XmlData(213) and EncodedText(355) may contain
// SOH: they are read using their preceding length field.
#include "fastmm/codecs/fix/fix_tags.hpp"
#include "fastmm/codecs/fix/fix_time.hpp"
#include "fastmm/core/fixed_point.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace fastmm::codecs::fix {

enum class FixError : std::uint8_t {
  None = 0,
  Truncated = 1,       // shorter than the smallest possible message
  BadBeginString = 2,  // 8= missing, empty or not the expected version
  BadBodyLength = 3,   // 9= missing/malformed, or does not end at "10="
  BadMsgType = 4,      // 35= is not the third field
  BadField = 5,        // tag not a positive integer, missing '=', empty value
  BadCheckSum = 6,     // 10= malformed or wrong
  TooManyFields = 7,   // more than FixView::kMaxFields
};
[[nodiscard]] constexpr std::string_view to_string(FixError e) noexcept {
  switch (e) {
    case FixError::None:
      return "None";
    case FixError::Truncated:
      return "Truncated";
    case FixError::BadBeginString:
      return "BadBeginString";
    case FixError::BadBodyLength:
      return "BadBodyLength";
    case FixError::BadMsgType:
      return "BadMsgType";
    case FixError::BadField:
      return "BadField";
    case FixError::BadCheckSum:
      return "BadCheckSum";
    case FixError::TooManyFields:
      return "TooManyFields";
  }
  return "?";
}

// ---- value parsers (FIX 4.4 data types) --------------------------------------------------

// int: "Sequence of digits without commas or decimals and optional sign character".
[[nodiscard]] constexpr std::optional<std::int64_t> parse_fix_int(std::string_view s) noexcept {
  if (s.empty()) return std::nullopt;
  std::size_t i = 0;
  const bool neg = s[0] == '-';
  if (neg) i = 1;
  if (i == s.size() || s.size() - i > 18) return std::nullopt;
  std::int64_t v = 0;
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c < '0' || c > '9') return std::nullopt;
    v = v * 10 + (c - '0');
  }
  return neg ? -v : v;
}
[[nodiscard]] constexpr std::optional<char> parse_fix_char(std::string_view s) noexcept {
  if (s.size() != 1) return std::nullopt;
  return s[0];
}
// Boolean: 'Y' or 'N'.
[[nodiscard]] constexpr std::optional<bool> parse_fix_bool(std::string_view s) noexcept {
  if (s.size() != 1 || (s[0] != 'Y' && s[0] != 'N')) return std::nullopt;
  return s[0] == 'Y';
}
// float/Price/Qty: "Sequence of digits with optional decimal point and sign character"
// (no '+', no exponent). Exact: at most 8 significant fraction digits.
template <class Tag>
[[nodiscard]] constexpr std::optional<Fixed<Tag>> parse_fix_decimal(std::string_view s) noexcept {
  if (s.empty() || s[0] == '+') return std::nullopt;
  return Fixed<Tag>::from_decimal(s);
}

struct FixField {
  std::uint32_t tag = 0;
  std::string_view value;
};

class FixView;

// A contiguous run of fields [begin, end) of a parsed message: the whole message or one entry of
// a repeating group. Lookups return the first occurrence inside the run.
class FixRange {
 public:
  FixRange() noexcept = default;
  FixRange(const FixView* v, std::uint32_t begin, std::uint32_t end) noexcept
      : view_(v), begin_(begin), end_(end) {}

  [[nodiscard]] std::uint32_t begin_index() const noexcept { return begin_; }
  [[nodiscard]] std::uint32_t end_index() const noexcept { return end_; }
  [[nodiscard]] std::uint32_t size() const noexcept { return end_ - begin_; }

  [[nodiscard]] inline std::string_view get(std::uint32_t tag) const noexcept;
  [[nodiscard]] bool has(std::uint32_t tag) const noexcept { return !get(tag).empty(); }
  [[nodiscard]] std::optional<std::int64_t> get_int(std::uint32_t tag) const noexcept {
    return parse_fix_int(get(tag));
  }
  [[nodiscard]] std::optional<char> get_char(std::uint32_t tag) const noexcept {
    return parse_fix_char(get(tag));
  }
  [[nodiscard]] std::optional<bool> get_bool(std::uint32_t tag) const noexcept {
    return parse_fix_bool(get(tag));
  }
  [[nodiscard]] std::optional<Price> get_price(std::uint32_t tag) const noexcept {
    return parse_fix_decimal<PriceTag>(get(tag));
  }
  [[nodiscard]] std::optional<Qty> get_qty(std::uint32_t tag) const noexcept {
    return parse_fix_decimal<QtyTag>(get(tag));
  }
  [[nodiscard]] std::optional<std::int64_t> get_timestamp_ns(std::uint32_t tag) const noexcept {
    return parse_utc_timestamp(get(tag));
  }

 private:
  const FixView* view_ = nullptr;
  std::uint32_t begin_ = 0;
  std::uint32_t end_ = 0;
};

class FixView {
 public:
  static constexpr std::size_t kMaxFields = 1024;
  static constexpr std::uint32_t kDirectTags = 1024;  // O(1) lookup for tags below this
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  FixView() noexcept = default;

  // Validates and indexes `msg` (exactly one message, CheckSum field included). An empty
  // `begin_string` accepts any BeginString. The view refers to `msg`: keep the bytes alive.
  FixError parse(std::string_view msg,
                 std::string_view begin_string = kBeginString44,
                 bool verify_checksum = true) noexcept;
  FixError parse(std::span<const std::byte> msg,
                 std::string_view begin_string = kBeginString44,
                 bool verify_checksum = true) noexcept {
    return parse(std::string_view(reinterpret_cast<const char*>(msg.data()), msg.size()),
                 begin_string,
                 verify_checksum);
  }

  [[nodiscard]] bool valid() const noexcept { return valid_; }
  [[nodiscard]] std::string_view raw() const noexcept { return msg_; }
  [[nodiscard]] std::string_view begin_string() const noexcept { return field(0).value; }
  [[nodiscard]] std::string_view msg_type() const noexcept { return msg_type_; }
  // Byte offsets: first byte after "9=<n>|" and the start of "10=".
  [[nodiscard]] std::size_t body_offset() const noexcept { return body_off_; }
  [[nodiscard]] std::size_t trailer_offset() const noexcept { return trailer_off_; }

  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] FixField field(std::size_t i) const noexcept {
    const Entry& e = entries_[i];
    return {e.tag, std::string_view(msg_.data() + e.off, e.len)};
  }
  // Index of the first `tag` at or after `from`, npos when absent.
  [[nodiscard]] std::size_t find(std::uint32_t tag, std::size_t from = 0) const noexcept {
    if (tag < kDirectTags && from == 0) {
      const std::uint16_t d = direct_[tag];
      return d == 0 ? npos : static_cast<std::size_t>(d - 1U);
    }
    for (std::size_t i = from; i < count_; ++i) {
      if (entries_[i].tag == tag) return i;
    }
    return npos;
  }
  [[nodiscard]] FixRange all() const noexcept {
    return {this, 0, static_cast<std::uint32_t>(count_)};
  }

  [[nodiscard]] std::string_view get(std::uint32_t tag) const noexcept {
    const std::size_t i = find(tag);
    return i == npos ? std::string_view{} : field(i).value;
  }
  [[nodiscard]] bool has(std::uint32_t tag) const noexcept { return find(tag) != npos; }
  [[nodiscard]] std::optional<std::int64_t> get_int(std::uint32_t tag) const noexcept {
    return parse_fix_int(get(tag));
  }
  [[nodiscard]] std::optional<char> get_char(std::uint32_t tag) const noexcept {
    return parse_fix_char(get(tag));
  }
  [[nodiscard]] std::optional<bool> get_bool(std::uint32_t tag) const noexcept {
    return parse_fix_bool(get(tag));
  }
  [[nodiscard]] std::optional<Price> get_price(std::uint32_t tag) const noexcept {
    return parse_fix_decimal<PriceTag>(get(tag));
  }
  [[nodiscard]] std::optional<Qty> get_qty(std::uint32_t tag) const noexcept {
    return parse_fix_decimal<QtyTag>(get(tag));
  }
  [[nodiscard]] std::optional<std::int64_t> get_timestamp_ns(std::uint32_t tag) const noexcept {
    return parse_utc_timestamp(get(tag));
  }

  // Scans a raw message for CheckSum purposes: sum of bytes modulo 256.
  [[nodiscard]] static std::uint8_t checksum(std::string_view bytes) noexcept {
    std::uint32_t sum = 0;
    for (const char c : bytes) sum += static_cast<unsigned char>(c);
    return static_cast<std::uint8_t>(sum & 0xFFU);
  }

 private:
  struct Entry {
    std::uint32_t tag;
    std::uint32_t off;
    std::uint32_t len;
  };
  bool add(std::uint32_t tag, std::size_t off, std::size_t len) noexcept {
    if (count_ >= kMaxFields) return false;
    entries_[count_] = Entry{tag, static_cast<std::uint32_t>(off), static_cast<std::uint32_t>(len)};
    if (tag < kDirectTags && direct_[tag] == 0)
      direct_[tag] = static_cast<std::uint16_t>(count_ + 1);
    ++count_;
    return true;
  }
  void clear_index() noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (entries_[i].tag < kDirectTags) direct_[entries_[i].tag] = 0;
    }
    count_ = 0;
  }

  std::string_view msg_;
  std::string_view msg_type_;
  std::size_t count_ = 0;
  std::size_t body_off_ = 0;
  std::size_t trailer_off_ = 0;
  bool valid_ = false;
  Entry entries_[kMaxFields]{};
  std::uint16_t direct_[kDirectTags]{};
};

inline std::string_view FixRange::get(std::uint32_t tag) const noexcept {
  if (view_ == nullptr) return {};
  if (begin_ == 0 && end_ == view_->size()) return view_->get(tag);
  for (std::uint32_t i = begin_; i < end_; ++i) {
    const FixField f = view_->field(i);
    if (f.tag == tag) return f.value;
  }
  return {};
}

// Iterates the entries of a repeating group. The group starts after its NumInGroup field
// `count_tag`; every entry starts with `first_tag` (FIX requires the first field of an entry to
// be present and first). An entry ends at the next `first_tag`, at a tag not in `members`
// (when `members` is non-empty) or at CheckSum. complete() tells whether exactly NumInGroup
// entries were found (SessionRejectReason 16 otherwise).
class FixGroupReader {
 public:
  FixGroupReader(const FixView& v,
                 std::uint32_t count_tag,
                 std::uint32_t first_tag,
                 std::span<const std::uint32_t> members = {}) noexcept
      : view_(&v), first_tag_(first_tag), members_(members) {
    const std::size_t i = v.find(count_tag);
    end_ = v.size() == 0 ? 0 : static_cast<std::uint32_t>(v.size() - 1);  // stop at CheckSum
    if (i == FixView::npos) {
      pos_ = end_;
      return;
    }
    present_ = true;
    declared_ = parse_fix_int(v.field(i).value).value_or(-1);
    pos_ = static_cast<std::uint32_t>(i + 1);
  }

  [[nodiscard]] bool present() const noexcept { return present_; }
  [[nodiscard]] std::int64_t declared() const noexcept { return declared_; }
  [[nodiscard]] std::int64_t read() const noexcept { return read_; }
  [[nodiscard]] bool complete() const noexcept {
    return present_ && declared_ >= 0 && read_ == declared_;
  }

  bool next(FixRange& out) noexcept {
    if (read_ >= declared_ || pos_ >= end_ || view_->field(pos_).tag != first_tag_) return false;
    const std::uint32_t start = pos_++;
    while (pos_ < end_) {
      const std::uint32_t t = view_->field(pos_).tag;
      if (t == first_tag_ || !member(t)) break;
      ++pos_;
    }
    out = FixRange(view_, start, pos_);
    ++read_;
    return true;
  }

 private:
  [[nodiscard]] bool member(std::uint32_t t) const noexcept {
    if (members_.empty()) return true;
    for (const std::uint32_t m : members_) {
      if (m == t) return true;
    }
    return false;
  }

  const FixView* view_;
  std::uint32_t first_tag_;
  std::span<const std::uint32_t> members_;
  std::uint32_t pos_ = 0;
  std::uint32_t end_ = 0;
  std::int64_t declared_ = 0;
  std::int64_t read_ = 0;
  bool present_ = false;
};

}  // namespace fastmm::codecs::fix
