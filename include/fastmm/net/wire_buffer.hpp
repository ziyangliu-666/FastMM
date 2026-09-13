#pragma once
// Fixed-capacity send buffer. Bytes are appended at the tail and drained from the head as
// the socket accepts them; the unsent region is pending(). Single allocation at construction,
// never grows: a full buffer is a back-pressure signal (append returns false).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>

namespace fastmm::net {

class WireBuffer {
 public:
  explicit WireBuffer(std::size_t capacity)
      : capacity_(capacity), data_(std::make_unique<std::byte[]>(capacity)) {}

  std::size_t capacity() const noexcept { return capacity_; }
  std::size_t size() const noexcept { return wr_ - rd_; }
  std::size_t free_space() const noexcept { return capacity_ - wr_; }
  bool empty() const noexcept { return rd_ == wr_; }

  // Contiguous free space large enough for `n` bytes, compacting if needed. Returns false
  // if even a compacted buffer cannot hold it.
  bool reserve(std::size_t n) noexcept {
    if (capacity_ - wr_ >= n) return true;
    if (capacity_ - size() < n) return false;
    compact();
    return true;
  }

  bool append(std::span<const std::byte> bytes) noexcept {
    if (bytes.empty()) return true;  // std::span{} may carry a null pointer
    if (!reserve(bytes.size())) return false;
    std::memcpy(data_.get() + wr_, bytes.data(), bytes.size());
    wr_ += bytes.size();
    return true;
  }
  bool append(std::string_view s) noexcept {
    return append(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size()));
  }
  bool append_u8(std::uint8_t v) noexcept {
    if (!reserve(1)) return false;
    data_[wr_++] = static_cast<std::byte>(v);
    return true;
  }

  // Raw write cursor for encoders that emit directly (e.g. frame headers); the caller must
  // have called reserve(n) and then commit(n).
  std::byte* write_ptr() noexcept { return data_.get() + wr_; }
  void commit(std::size_t n) noexcept { wr_ += n; }

  // XOR-masks [offset, offset + len) of the *pending* region with a 4-byte WebSocket mask,
  // processing 8 bytes per step. offset is relative to pending().data().
  void mask_xor_inplace(std::size_t offset, std::size_t len, const std::uint8_t mask[4]) noexcept {
    std::byte* p = data_.get() + rd_ + offset;
    std::uint64_t m64 = 0;
    std::uint8_t m8[8] = {mask[0], mask[1], mask[2], mask[3], mask[0], mask[1], mask[2], mask[3]};
    std::memcpy(&m64, m8, 8);
    std::size_t i = 0;
    for (; i + 8 <= len; i += 8) {
      std::uint64_t v = 0;
      std::memcpy(&v, p + i, 8);
      v ^= m64;
      std::memcpy(p + i, &v, 8);
    }
    for (; i < len; ++i) p[i] ^= static_cast<std::byte>(mask[i & 3]);
  }

  std::span<const std::byte> pending() const noexcept { return {data_.get() + rd_, wr_ - rd_}; }

  // The socket accepted `n` bytes from the front of pending().
  void consumed(std::size_t n) noexcept {
    rd_ += n;
    if (rd_ == wr_) rd_ = wr_ = 0;
  }
  void clear() noexcept { rd_ = wr_ = 0; }

 private:
  void compact() noexcept {
    const std::size_t n = wr_ - rd_;
    std::memmove(data_.get(), data_.get() + rd_, n);
    rd_ = 0;
    wr_ = n;
  }

  std::size_t capacity_;
  std::unique_ptr<std::byte[]> data_;
  std::size_t rd_ = 0;
  std::size_t wr_ = 0;
};

}  // namespace fastmm::net
