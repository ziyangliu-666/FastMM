#pragma once
// Pool<T, N>: fixed-size object pool handing out 32-bit Handle<T> indices. Freed slots form
// an intrusive singly-linked list threaded through the slot storage itself, taken first, then
// the slots never used, in index order; so allocate and free are O(1) with no extra memory beyond
// a liveness bitmap (used for assertions and deterministic handle-order iteration), and a slot
// is not touched before it is first used (warm_up() touches them all).
//
// T must be trivially copyable (slots are reused via a union).
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/strong_id.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace fastmm {

template <class T, std::size_t N>
class Pool {
  static_assert(std::is_trivially_copyable_v<T>, "Pool requires trivially copyable T");
  static_assert(N > 0 && N < kNullHandle);

 public:
  using handle_type = Handle<T>;
  static constexpr std::size_t kCapacity = N;

  Pool() : slots_(new Slot[N]), live_(new std::uint64_t[kWords]()) { reset(); }
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;

  void reset() noexcept {
    for (std::size_t w = 0; w < kWords; ++w) live_[w] = 0;
    free_head_ = kNullHandle;
    unused_ = 0;
    size_ = 0;
  }

  // Touches every slot never used so the pages are resident before the hot loop starts.
  void warm_up() noexcept {
    for (std::size_t i = unused_; i < N; ++i) slots_[i].next_free = 0;
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }
  [[nodiscard]] bool full() const noexcept { return size_ == N; }

  // Returns a null handle when exhausted. The slot contents are indeterminate; callers
  // assign a fully initialised T.
  [[nodiscard]] FASTMM_FORCE_INLINE handle_type allocate() noexcept {
    std::uint32_t h = free_head_;
    if (h != kNullHandle) {
      free_head_ = slots_[h].next_free;
    } else if (FASTMM_LIKELY(unused_ < N)) {
      h = unused_++;
    } else {
      return handle_type{};
    }
    live_[h / 64] |= (1ULL << (h % 64));
    ++size_;
    return handle_type{h};
  }
  [[nodiscard]] handle_type allocate(const T& init) noexcept {
    const handle_type h = allocate();
    if (h.valid()) slots_[h.idx].obj = init;
    return h;
  }
  FASTMM_FORCE_INLINE void free(handle_type h) noexcept {
    FASTMM_ASSERT(is_live(h));
    live_[h.idx / 64] &= ~(1ULL << (h.idx % 64));
    slots_[h.idx].next_free = free_head_;
    free_head_ = h.idx;
    --size_;
  }

  [[nodiscard]] FASTMM_FORCE_INLINE T& get(handle_type h) noexcept {
    FASTMM_ASSERT(is_live(h));
    return slots_[h.idx].obj;
  }
  [[nodiscard]] FASTMM_FORCE_INLINE const T& get(handle_type h) const noexcept {
    FASTMM_ASSERT(is_live(h));
    return slots_[h.idx].obj;
  }
  T& operator[](handle_type h) noexcept { return get(h); }
  const T& operator[](handle_type h) const noexcept { return get(h); }

  [[nodiscard]] bool is_live(handle_type h) const noexcept {
    return h.idx < N && ((live_[h.idx / 64] >> (h.idx % 64)) & 1U) != 0;
  }
  [[nodiscard]] handle_type handle_of(const T& obj) const noexcept {
    const auto* s = reinterpret_cast<const Slot*>(&obj);
    return handle_type{static_cast<std::uint32_t>(s - slots_.get())};
  }

  // Visits live objects in ascending handle order (deterministic). F(handle_type, T&).
  // F may free the visited handle but must not allocate.
  template <class F>
  void for_each(F&& f) noexcept {
    for (std::size_t w = 0; w < kWords; ++w) {
      std::uint64_t bits = live_[w];
      while (bits != 0) {
        const auto bit = static_cast<std::uint32_t>(__builtin_ctzll(bits));
        bits &= bits - 1;
        const auto idx = static_cast<std::uint32_t>(w * 64 + bit);
        f(handle_type{idx}, slots_[idx].obj);
      }
    }
  }
  template <class F>
  void for_each(F&& f) const noexcept {
    for (std::size_t w = 0; w < kWords; ++w) {
      std::uint64_t bits = live_[w];
      while (bits != 0) {
        const auto bit = static_cast<std::uint32_t>(__builtin_ctzll(bits));
        bits &= bits - 1;
        const auto idx = static_cast<std::uint32_t>(w * 64 + bit);
        f(handle_type{idx}, slots_[idx].obj);
      }
    }
  }

 private:
  static constexpr std::size_t kWords = (N + 63) / 64;
  union Slot {
    T obj;
    std::uint32_t next_free;
    Slot() noexcept {}  // NOLINT(modernize-use-equals-default): leaves the slot untouched
  };

  std::unique_ptr<Slot[]> slots_;
  std::unique_ptr<std::uint64_t[]> live_;
  std::uint32_t free_head_ = kNullHandle;
  std::uint32_t unused_ = 0;  // slots [unused_, N) were never handed out
  std::size_t size_ = 0;
};

}  // namespace fastmm
