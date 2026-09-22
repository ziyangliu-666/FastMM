#pragma once
// CounterKeyMap<V, N, OverflowN>: a map for non-zero keys handed out by a counter (ClientOrderId
// sequence numbers, OUCH UserRefNums). Key k lives in slot k mod N unless an older key still
// holds that slot (one at least N keys older, or from another counter), in which case it goes
// to a small OpenHashMap. Hits and erases touch one slot, consecutive keys share cache lines,
// and there are no probe chains: an OpenHashMap over such keys either scatters them (a cache
// miss per order) or, with low-bit homes, packs every live key into one cluster that each
// backward-shift erase walks to its end.
//
// No iteration API, like OpenHashMap. Never allocates after construction.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/hot_array.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace fastmm {

template <class V, std::size_t N, std::size_t OverflowN = 4096>
class CounterKeyMap {
  static_assert(std::has_single_bit(N));
  static_assert(std::is_trivially_copyable_v<V>);

 public:
  static constexpr std::size_t kCapacity = N;

  CounterKeyMap() : slots_(make_hot_array<Slot>(N)) {}
  CounterKeyMap(const CounterKeyMap&) = delete;
  CounterKeyMap& operator=(const CounterKeyMap&) = delete;

  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  [[nodiscard]] FASTMM_FORCE_INLINE V* find(std::uint64_t k) noexcept {
    Slot& s = slots_[k & (N - 1)];
    if (FASTMM_LIKELY(s.key == k && k != 0)) return &s.value;
    return overflow_.empty() ? nullptr : overflow_.find(k);
  }
  [[nodiscard]] FASTMM_FORCE_INLINE const V* find(std::uint64_t k) const noexcept {
    return const_cast<CounterKeyMap*>(this)->find(k);
  }

  // `k` must be non-zero and absent. False when both its slot and the overflow table are taken.
  bool insert(std::uint64_t k, const V& v) noexcept {
    if (k == 0) return false;
    Slot& s = slots_[k & (N - 1)];
    if (FASTMM_LIKELY(s.key == 0)) {
      s.key = k;
      s.value = v;
      ++size_;
      return true;
    }
    const auto [p, inserted] = overflow_.insert(k, v);
    if (p == nullptr || !inserted) return false;
    ++size_;
    return true;
  }

  bool erase(std::uint64_t k) noexcept {
    Slot& s = slots_[k & (N - 1)];
    if (s.key == k && k != 0) {
      s.key = 0;
      --size_;
      return true;
    }
    if (!overflow_.empty() && overflow_.erase(k)) {
      --size_;
      return true;
    }
    return false;
  }

 private:
  struct Slot {
    std::uint64_t key;  // 0: empty
    V value;
  };

  HotArray<Slot> slots_;
  OpenHashMap<std::uint64_t, V, OverflowN> overflow_;
  std::size_t size_ = 0;
};

}  // namespace fastmm
