#pragma once
// FlatMap<K, V, N>: sorted fixed-capacity array map with binary search. Meant for small,
// read-mostly tables built at startup (symbol -> InstrumentId). Keys need operator<=>.
#include "fastmm/core/config_macros.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

namespace fastmm {

template <class K, class V, std::size_t N>
class FlatMap {
  static_assert(std::is_trivially_copyable_v<K> && std::is_trivially_copyable_v<V>);

 public:
  struct Entry {
    K key;
    V value;
  };

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  void clear() noexcept { size_ = 0; }

  // Returns {pointer, inserted}. Existing key -> pointer to old value, inserted=false.
  // Full -> {nullptr, false}.
  std::pair<V*, bool> insert(const K& k, const V& v) noexcept {
    const std::size_t i = lower_bound(k);
    if (i < size_ && entries_[i].key == k) return {&entries_[i].value, false};
    if (size_ == N) return {nullptr, false};
    if (i < size_) std::memmove(entries_ + i + 1, entries_ + i, (size_ - i) * sizeof(Entry));
    entries_[i].key = k;
    entries_[i].value = v;
    ++size_;
    return {&entries_[i].value, true};
  }
  // Insert or overwrite.
  V* assign(const K& k, const V& v) noexcept {
    auto [p, inserted] = insert(k, v);
    if (p != nullptr && !inserted) *p = v;
    return p;
  }
  [[nodiscard]] V* find(const K& k) noexcept {
    const std::size_t i = lower_bound(k);
    return (i < size_ && entries_[i].key == k) ? &entries_[i].value : nullptr;
  }
  [[nodiscard]] const V* find(const K& k) const noexcept {
    const std::size_t i = lower_bound(k);
    return (i < size_ && entries_[i].key == k) ? &entries_[i].value : nullptr;
  }
  [[nodiscard]] bool contains(const K& k) const noexcept { return find(k) != nullptr; }
  bool erase(const K& k) noexcept {
    const std::size_t i = lower_bound(k);
    if (i >= size_ || !(entries_[i].key == k)) return false;
    if (i + 1 < size_)
      std::memmove(entries_ + i, entries_ + i + 1, (size_ - i - 1) * sizeof(Entry));
    --size_;
    return true;
  }

  const Entry* begin() const noexcept { return entries_; }
  const Entry* end() const noexcept { return entries_ + size_; }

 private:
  [[nodiscard]] std::size_t lower_bound(const K& k) const noexcept {
    std::size_t lo = 0;
    std::size_t hi = size_;
    while (lo < hi) {
      const std::size_t mid = lo + (hi - lo) / 2;
      if (entries_[mid].key < k) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return lo;
  }

  std::size_t size_ = 0;
  Entry entries_[N];
};

}  // namespace fastmm
