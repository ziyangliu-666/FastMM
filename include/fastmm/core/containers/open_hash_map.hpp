#pragma once
// OpenHashMap<K, V, N>: open addressing with linear probing and backward-shift deletion
// (no tombstones, so lookups never degrade). Capacity N is a power of two and fixed at
// compile time; insert fails deterministically above 7/8 load rather than rehashing.
//
// Deliberately has no iteration API (5.11): iteration order would depend on hash layout
// and break replay determinism. Iterate the owning Pool in handle order instead.
//
// Keys are integral or StrongId-like (a `.value` integral member).
//
// Layout: the occupancy flag lives in the slot, so a probe touches one cache line, not a
// slot line plus a line of a separate flag array. The table is a HotArray: zeroed and
// resident at construction (no page fault on the first insert into a page), on huge pages
// from 1 MiB.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/hot_array.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace fastmm {

namespace detail {
template <class K>
struct KeyRep {
  static constexpr std::uint64_t to_u64(const K& k) noexcept {
    if constexpr (std::is_integral_v<K>) {
      return static_cast<std::uint64_t>(k);
    } else {
      return static_cast<std::uint64_t>(k.value);
    }
  }
};
}  // namespace detail

template <class K, class V, std::size_t N>
class OpenHashMap {
  static_assert(std::has_single_bit(N) && N >= 4, "capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<K> && std::is_trivially_copyable_v<V>);

 public:
  static constexpr std::size_t kCapacity = N;
  static constexpr std::size_t kMaxSize = N - N / 8;  // 87.5 % load cap

  // Construction happens at startup only; failing to allocate the table is fatal by design.
  // NOLINTNEXTLINE(bugprone-unhandled-exception-at-new)
  OpenHashMap() noexcept : slots_(make_hot_array<Slot>(N)) {}
  OpenHashMap(const OpenHashMap&) = delete;
  OpenHashMap& operator=(const OpenHashMap&) = delete;

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }

  void clear() noexcept {
    for (std::size_t i = 0; i < N; ++i) slots_[i].used = 0;
    size_ = 0;
  }

  [[nodiscard]] FASTMM_FORCE_INLINE V* find(const K& k) noexcept {
    std::size_t i = index_of(k);
    while (slots_[i].used != 0) {
      if (slots_[i].key == k) return &slots_[i].value;
      i = (i + 1) & (N - 1);
    }
    return nullptr;
  }
  [[nodiscard]] FASTMM_FORCE_INLINE const V* find(const K& k) const noexcept {
    return const_cast<OpenHashMap*>(this)->find(k);
  }
  [[nodiscard]] bool contains(const K& k) const noexcept { return find(k) != nullptr; }

  // {pointer to value, inserted}. Existing key -> old value, false. Full -> {nullptr,false}.
  std::pair<V*, bool> insert(const K& k, const V& v) noexcept {
    std::size_t i = index_of(k);
    while (slots_[i].used != 0) {
      if (slots_[i].key == k) return {&slots_[i].value, false};
      i = (i + 1) & (N - 1);
    }
    if (FASTMM_UNLIKELY(size_ >= kMaxSize)) return {nullptr, false};
    slots_[i].used = 1;
    slots_[i].key = k;
    slots_[i].value = v;
    ++size_;
    return {&slots_[i].value, true};
  }
  V* assign(const K& k, const V& v) noexcept {
    auto [p, inserted] = insert(k, v);
    if (p != nullptr && !inserted) *p = v;
    return p;
  }

  bool erase(const K& k) noexcept {
    std::size_t i = index_of(k);
    while (slots_[i].used != 0) {
      if (slots_[i].key == k) {
        erase_slot(i);
        return true;
      }
      i = (i + 1) & (N - 1);
    }
    return false;
  }

 private:
  struct Slot {
    K key;
    V value;
    std::uint8_t used;  // usually in what would be tail padding
  };

  [[nodiscard]] static FASTMM_FORCE_INLINE std::size_t index_of(const K& k) noexcept {
    // Fibonacci hashing: multiply by 2^64/phi and take the top log2(N) bits.
    const std::uint64_t h = detail::KeyRep<K>::to_u64(k) * 0x9E3779B97F4A7C15ULL;
    return h >> (64 - std::countr_zero(N));
  }

  // Backward-shift deletion: pull subsequent probe-chain entries down into the hole as
  // long as their home slot is not "after" the hole in circular order.
  void erase_slot(std::size_t hole) noexcept {
    std::size_t j = hole;
    for (;;) {
      j = (j + 1) & (N - 1);
      if (slots_[j].used == 0) break;
      const std::size_t home = index_of(slots_[j].key);
      // Slot j may move into `hole` iff home is not in the cyclic range (hole, j].
      const bool in_range = hole <= j ? (home > hole && home <= j) : (home > hole || home <= j);
      if (!in_range) {
        slots_[hole] = slots_[j];
        hole = j;
      }
    }
    slots_[hole].used = 0;
    --size_;
  }

  HotArray<Slot> slots_;
  std::size_t size_ = 0;
};

}  // namespace fastmm
