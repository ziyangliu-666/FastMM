#pragma once
// JsonWriter: append-only JSON builder over a caller-provided fixed buffer (no heap). Used by
// the order encoders on the hot path. Overflow is sticky (ok() == false, size() frozen), so
// a truncated request can never be sent. Strings are escaped minimally (quote, backslash,
// control chars) - venue symbols and ids are plain ASCII anyway.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace fastmm::venues {

class JsonWriter {
 public:
  explicit JsonWriter(std::span<char> buf) noexcept : buf_(buf) {}

  JsonWriter& begin_object() noexcept { return sep_value().raw_char('{').open(); }
  JsonWriter& end_object() noexcept { return raw_char('}').close(); }
  JsonWriter& begin_array() noexcept { return sep_value().raw_char('[').open(); }
  JsonWriter& end_array() noexcept { return raw_char(']').close(); }

  JsonWriter& key(std::string_view k) noexcept {
    sep_value();
    put_string(k);
    raw_char(':');
    pending_key_ = true;  // the value that follows must not be preceded by a comma
    return *this;
  }
  JsonWriter& string(std::string_view v) noexcept {
    sep_value();
    put_string(v);
    return *this;
  }
  JsonWriter& integer(std::int64_t v) noexcept {
    sep_value();
    char tmp[21];
    std::size_t n = 0;
    std::uint64_t mag = v < 0 ? 0 - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
    char d[20];
    std::size_t k = 0;
    do {
      d[k++] = static_cast<char>('0' + mag % 10);
      mag /= 10;
    } while (mag != 0);
    if (v < 0) tmp[n++] = '-';
    while (k > 0) tmp[n++] = d[--k];
    return raw(std::string_view(tmp, n));
  }
  JsonWriter& boolean(bool v) noexcept {
    sep_value();
    return raw(v ? "true" : "false");
  }
  JsonWriter& null() noexcept {
    sep_value();
    return raw("null");
  }
  // Pre-formatted JSON fragment (e.g. a nested object built elsewhere).
  JsonWriter& raw_value(std::string_view v) noexcept {
    sep_value();
    return raw(v);
  }

  [[nodiscard]] std::string_view view() const noexcept { return {buf_.data(), len_}; }
  [[nodiscard]] std::size_t size() const noexcept { return len_; }
  [[nodiscard]] bool ok() const noexcept { return ok_; }
  void clear() noexcept {
    len_ = 0;
    ok_ = true;
    first_ = true;
    depth_ = 0;
    pending_key_ = false;
  }

 private:
  JsonWriter& sep_value() noexcept {
    if (pending_key_) {
      pending_key_ = false;
      return *this;
    }
    if (!first_) raw_char(',');
    first_ = false;
    return *this;
  }
  JsonWriter& open() noexcept {
    first_ = true;
    ++depth_;
    return *this;
  }
  JsonWriter& close() noexcept {
    first_ = false;
    if (depth_ > 0) --depth_;
    return *this;
  }
  void put_string(std::string_view s) noexcept {
    raw_char('"');
    for (char c : s) {
      switch (c) {
        case '"':
          raw("\\\"");
          break;
        case '\\':
          raw("\\\\");
          break;
        case '\n':
          raw("\\n");
          break;
        case '\r':
          raw("\\r");
          break;
        case '\t':
          raw("\\t");
          break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            static constexpr char kHex[] = "0123456789abcdef";
            raw("\\u00");
            raw_char(kHex[(static_cast<unsigned char>(c) >> 4) & 0xF]);
            raw_char(kHex[static_cast<unsigned char>(c) & 0xF]);
          } else {
            raw_char(c);
          }
      }
    }
    raw_char('"');
  }
  JsonWriter& raw(std::string_view s) noexcept {
    for (char c : s) raw_char(c);
    return *this;
  }
  JsonWriter& raw_char(char c) noexcept {
    if (!ok_) return *this;
    if (len_ >= buf_.size()) {
      ok_ = false;
      return *this;
    }
    buf_[len_++] = c;
    return *this;
  }

  std::span<char> buf_;
  std::size_t len_ = 0;
  bool ok_ = true;
  bool first_ = true;
  bool pending_key_ = false;
  int depth_ = 0;
};

}  // namespace fastmm::venues
