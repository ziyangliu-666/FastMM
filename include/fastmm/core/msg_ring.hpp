#pragma once
// MsgRing: byte-oriented SPSC ring for variable-length, 64-byte-granular messages.
//
// Protocol: every message starts with a 4-byte little-endian length (bytes) followed by a
// 1-byte type; EventHeader in messages.hpp has exactly this prefix. Lengths are multiples
// of 64 so every message is cache-line aligned in the ring. When a message would straddle
// the end of the buffer the producer writes a Padding message (type 0) covering the tail
// and starts the real message at offset 0; the consumer skips padding transparently.
//
//   producer:  if (auto* p = ring.try_reserve(len)) { fill(p); ring.commit(); }
//   consumer:  while (const auto* m = ring.try_peek()) { handle(m); ring.release(); }
//
// Backing memory is either owned (aligned new at construction) or borrowed from an Arena.
#include "fastmm/core/arena.hpp"
#include "fastmm/core/config_macros.hpp"

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

namespace fastmm {

struct RingMsgPrefix {
  std::uint32_t len;
  std::uint8_t type;
};
inline constexpr std::uint8_t kRingPaddingType = 0;
inline constexpr std::uint32_t kRingMsgGranule = 64;

class MsgRing {
 public:
  // Owns `capacity` bytes (power of two, >= 64).
  explicit MsgRing(std::size_t capacity)
      : owned_(static_cast<std::byte*>(::operator new(capacity, std::align_val_t{kCacheLine}))),
        buf_(owned_.get()),
        mask_(capacity - 1) {
    FASTMM_CHECK(std::has_single_bit(capacity) && capacity >= kRingMsgGranule);
    std::memset(buf_, 0, capacity);
  }
  // Borrows arena memory (pre-faulted, hugepage-advised).
  MsgRing(Arena& arena, std::size_t capacity)
      : buf_(static_cast<std::byte*>(arena.allocate(capacity, kCacheLine))), mask_(capacity - 1) {
    FASTMM_CHECK(std::has_single_bit(capacity) && capacity >= kRingMsgGranule);
    FASTMM_CHECK(buf_ != nullptr);
  }
  MsgRing(const MsgRing&) = delete;
  MsgRing& operator=(const MsgRing&) = delete;

  [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }

  // ---- producer ------------------------------------------------------------------------
  // Reserves `len` bytes (rounded up to 64). Returns nullptr if there is not enough free
  // space (including any padding needed to wrap). The returned pointer is 64-byte aligned.
  [[nodiscard]] std::byte* try_reserve(std::uint32_t len) noexcept {
    len = round_up(len);
    FASTMM_ASSERT(len <= capacity());
    const std::uint64_t t = tail_.load(std::memory_order_relaxed);
    const std::size_t off = t & mask_;
    const std::size_t to_end = capacity() - off;
    const std::uint64_t pad = to_end < len ? to_end : 0;  // to_end is 0 or >= 64
    const std::uint64_t need = pad + len;
    if (FASTMM_UNLIKELY(t + need - head_cache_ > capacity())) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (t + need - head_cache_ > capacity()) return nullptr;
    }
    if (pad != 0) {
      auto* p = reinterpret_cast<RingMsgPrefix*>(buf_ + off);
      p->len = static_cast<std::uint32_t>(pad);
      p->type = kRingPaddingType;
    }
    reserve_pos_ = t + pad;
    reserve_len_ = len;
    return buf_ + (reserve_pos_ & mask_);
  }
  // Publishes the message reserved by the last try_reserve. The caller must have written the
  // length prefix (== reserved length) into the first 4 bytes.
  FASTMM_FORCE_INLINE void commit() noexcept {
    FASTMM_ASSERT(reinterpret_cast<const RingMsgPrefix*>(buf_ + (reserve_pos_ & mask_))->len ==
                  reserve_len_);
    tail_.store(reserve_pos_ + reserve_len_, std::memory_order_release);
  }
  // Copy-in convenience: msg must begin with a valid prefix whose len == `len`.
  [[nodiscard]] bool try_push(const void* msg, std::uint32_t len) noexcept {
    std::byte* p = try_reserve(len);
    if (p == nullptr) return false;
    std::memcpy(p, msg, len);
    commit();
    return true;
  }

  // ---- consumer ------------------------------------------------------------------------
  // Pointer to the next message (64-byte aligned) or nullptr when empty. Padding is skipped.
  [[nodiscard]] const std::byte* try_peek() noexcept {
    for (;;) {
      const std::uint64_t h = head_.load(std::memory_order_relaxed);
      if (h == tail_cache_) {
        tail_cache_ = tail_.load(std::memory_order_acquire);
        if (h == tail_cache_) return nullptr;
      }
      const std::byte* p = buf_ + (h & mask_);
      const auto* pre = reinterpret_cast<const RingMsgPrefix*>(p);
      if (FASTMM_UNLIKELY(pre->type == kRingPaddingType)) {
        head_.store(h + pre->len, std::memory_order_release);
        continue;
      }
      return p;
    }
  }
  // Frees the message returned by the last try_peek.
  FASTMM_FORCE_INLINE void release() noexcept {
    const std::uint64_t h = head_.load(std::memory_order_relaxed);
    const auto* pre = reinterpret_cast<const RingMsgPrefix*>(buf_ + (h & mask_));
    head_.store(h + pre->len, std::memory_order_release);
  }

  [[nodiscard]] std::size_t bytes_used_approx() const noexcept {
    return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool empty_approx() const noexcept { return bytes_used_approx() == 0; }

 private:
  static constexpr std::uint32_t round_up(std::uint32_t len) noexcept {
    return (len + kRingMsgGranule - 1) & ~(kRingMsgGranule - 1);
  }
  struct Deleter {
    void operator()(std::byte* p) const noexcept {
      ::operator delete(p, std::align_val_t{kCacheLine});
    }
  };

  std::unique_ptr<std::byte, Deleter> owned_;
  std::byte* buf_;
  std::size_t mask_;

  alignas(kDestructiveInterference) std::atomic<std::uint64_t> tail_{0};
  std::uint64_t head_cache_ = 0;
  std::uint64_t reserve_pos_ = 0;
  std::uint32_t reserve_len_ = 0;
  alignas(kDestructiveInterference) std::atomic<std::uint64_t> head_{0};
  std::uint64_t tail_cache_ = 0;
};

}  // namespace fastmm
