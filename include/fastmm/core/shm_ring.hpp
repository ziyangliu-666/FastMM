#pragma once
// ShmRing: MsgRing's single-producer single-consumer protocol between two processes. The file
// (under /dev/shm) holds a header, the two shared indices on their own cache lines and the buffer;
// one process creates it, the other opens it, and each uses only its own side. Messages are the
// same 64-byte-granular, length-prefixed records with a Padding record at the wrap, so an engine
// reads a ShmRing exactly as it reads a MsgRing.
//
//   gateway:  auto r = ShmRing::create("/dev/shm/fastmm-gw-binance.md", 1 << 22);
//             r->try_push(&msg, msg.hdr.len);
//   engine:   auto r = ShmRing::open("/dev/shm/fastmm-gw-binance.md");
//             while (const auto* m = r->try_peek()) { handle(m); r->release(); }
//
// MsgRing itself keeps its indices inline: moving them behind a pointer cost the in-process engine
// step about 3% (2026-09-25), so the protocol is written twice and tests/core/shm_ring_test.cpp
// drives the same sequence through both.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/result.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace fastmm {

// The indices both processes share, each on its own cache line.
struct ShmRingControl {
  alignas(kDestructiveInterference) std::atomic<std::uint64_t> tail{0};
  alignas(kDestructiveInterference) std::atomic<std::uint64_t> head{0};
};

struct ShmRingHeader {
  static constexpr std::uint64_t kMagic = 0x474e495252534d46ULL;  // "FMSRRING"
  static constexpr std::uint32_t kVersion = 1;
  std::uint64_t magic;
  std::uint32_t version;
  std::uint32_t reserved;
  std::uint64_t capacity;  // buffer bytes, a power of two
};

class ShmRing {
 public:
  // Where the buffer starts in the file: one page for the header and the indices.
  static constexpr std::size_t kBufferOffset = 4096;

  // Creates (or truncates) `path` with an empty ring of `capacity` bytes (a power of two >= 64).
  [[nodiscard]] static Result<ShmRing, std::string> create(const std::string& path,
                                                           std::size_t capacity);
  // Maps a ring another process created.
  [[nodiscard]] static Result<ShmRing, std::string> open(const std::string& path);

  ShmRing(ShmRing&& o) noexcept;
  ShmRing& operator=(ShmRing&& o) noexcept;
  ShmRing(const ShmRing&) = delete;
  ShmRing& operator=(const ShmRing&) = delete;
  ~ShmRing();

  [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }

  // ---- producer (MsgRing::try_reserve / commit / try_push) --------------------------------
  [[nodiscard]] std::byte* try_reserve(std::uint32_t len) noexcept {
    len = round_up(len);
    FASTMM_ASSERT(len <= capacity());
    const std::uint64_t t = ctl_->tail.load(std::memory_order_relaxed);
    const std::size_t off = t & mask_;
    const std::size_t to_end = capacity() - off;
    const std::uint64_t pad = to_end < len ? to_end : 0;
    const std::uint64_t need = pad + len;
    if (FASTMM_UNLIKELY(t + need - head_cache_ > capacity())) {
      head_cache_ = ctl_->head.load(std::memory_order_acquire);
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
  FASTMM_FORCE_INLINE void commit() noexcept {
    FASTMM_ASSERT(reinterpret_cast<const RingMsgPrefix*>(buf_ + (reserve_pos_ & mask_))->len ==
                  reserve_len_);
    ctl_->tail.store(reserve_pos_ + reserve_len_, std::memory_order_release);
  }
  [[nodiscard]] bool try_push(const void* msg, std::uint32_t len) noexcept {
    std::byte* p = try_reserve(len);
    if (p == nullptr) return false;
    std::memcpy(p, msg, len);
    commit();
    return true;
  }

  // ---- consumer (MsgRing::try_peek / release) -----------------------------------------------
  [[nodiscard]] const std::byte* try_peek() noexcept {
    for (;;) {
      const std::uint64_t h = ctl_->head.load(std::memory_order_relaxed);
      if (h == tail_cache_) {
        tail_cache_ = ctl_->tail.load(std::memory_order_acquire);
        if (h == tail_cache_) return nullptr;
      }
      const std::byte* p = buf_ + (h & mask_);
      const auto* pre = reinterpret_cast<const RingMsgPrefix*>(p);
      if (FASTMM_UNLIKELY(pre->type == kRingPaddingType)) {
        ctl_->head.store(h + pre->len, std::memory_order_release);
        continue;
      }
      return p;
    }
  }
  FASTMM_FORCE_INLINE void release() noexcept {
    const std::uint64_t h = ctl_->head.load(std::memory_order_relaxed);
    const auto* pre = reinterpret_cast<const RingMsgPrefix*>(buf_ + (h & mask_));
    ctl_->head.store(h + pre->len, std::memory_order_release);
  }

  [[nodiscard]] std::size_t bytes_used_approx() const noexcept {
    return ctl_->tail.load(std::memory_order_acquire) - ctl_->head.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool empty_approx() const noexcept { return bytes_used_approx() == 0; }

 private:
  ShmRing() = default;
  static constexpr std::uint32_t round_up(std::uint32_t len) noexcept {
    return (len + kRingMsgGranule - 1) & ~(kRingMsgGranule - 1);
  }
  void unmap() noexcept;

  void* map_ = nullptr;
  std::size_t map_len_ = 0;
  ShmRingControl* ctl_ = nullptr;  // in the mapping, after the header
  std::byte* buf_ = nullptr;
  std::size_t mask_ = 0;
  // This process's side only: a ShmRing is used by one thread of one process.
  std::uint64_t head_cache_ = 0;
  std::uint64_t reserve_pos_ = 0;
  std::uint32_t reserve_len_ = 0;
  std::uint64_t tail_cache_ = 0;
};

}  // namespace fastmm
