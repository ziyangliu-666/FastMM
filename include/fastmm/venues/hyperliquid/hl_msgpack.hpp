#pragma once
// The subset of MessagePack Hyperliquid hashes actions with, encoded the way Python's
// msgpack.packb (1.x, use_bin_type) encodes the SDK's dicts: the smallest form of each integer,
// strings as fixstr / str8 / str16, maps and arrays as fix / 16-bit headers. Map keys go in the
// order the caller writes them, which must be the SDK's dict order: the hash covers it.
// Writes into a caller buffer; an overflow sets ok() false and stops writing.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace fastmm::venues::hyperliquid {

class MsgpackWriter {
 public:
  explicit MsgpackWriter(std::span<std::uint8_t> out) noexcept : out_(out) {}

  void map(std::uint32_t n) noexcept {
    if (n <= 15) {
      byte(static_cast<std::uint8_t>(0x80 | n));
    } else if (n <= 0xffff) {
      byte(0xde);
      be(n, 2);
    } else {
      byte(0xdf);
      be(n, 4);
    }
  }
  void array(std::uint32_t n) noexcept {
    if (n <= 15) {
      byte(static_cast<std::uint8_t>(0x90 | n));
    } else if (n <= 0xffff) {
      byte(0xdc);
      be(n, 2);
    } else {
      byte(0xdd);
      be(n, 4);
    }
  }
  void str(std::string_view s) noexcept {
    const std::size_t n = s.size();
    if (n <= 31) {
      byte(static_cast<std::uint8_t>(0xa0 | n));
    } else if (n <= 0xff) {
      byte(0xd9);
      be(n, 1);
    } else if (n <= 0xffff) {
      byte(0xda);
      be(n, 2);
    } else {
      byte(0xdb);
      be(n, 4);
    }
    raw(s);
  }
  void uint(std::uint64_t v) noexcept {
    if (v < 0x80) {
      byte(static_cast<std::uint8_t>(v));
    } else if (v <= 0xff) {
      byte(0xcc);
      be(v, 1);
    } else if (v <= 0xffff) {
      byte(0xcd);
      be(v, 2);
    } else if (v <= 0xffffffffULL) {
      byte(0xce);
      be(v, 4);
    } else {
      byte(0xcf);
      be(v, 8);
    }
  }
  void boolean(bool b) noexcept { byte(b ? 0xc3 : 0xc2); }
  void nil() noexcept { byte(0xc0); }

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] std::size_t size() const noexcept { return n_; }
  [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return out_.first(n_); }

 private:
  void byte(std::uint8_t b) noexcept {
    if (n_ >= out_.size()) {
      ok_ = false;
      return;
    }
    out_[n_++] = b;
  }
  void be(std::uint64_t v, int width) noexcept {
    for (int i = width - 1; i >= 0; --i) byte(static_cast<std::uint8_t>(v >> (8 * i)));
  }
  void raw(std::string_view s) noexcept {
    for (const char c : s) byte(static_cast<std::uint8_t>(c));
  }

  std::span<std::uint8_t> out_;
  std::size_t n_ = 0;
  bool ok_ = true;
};

}  // namespace fastmm::venues::hyperliquid
