#pragma once
// RecentMap<K, V, N>: the N most recently inserted keys. Inserting into a full map evicts the
// oldest key, so it never refuses and never grows: for lookups that only have to reach back a
// bounded distance (execution ids for deduplication, venue order ids for trade history).
//
// An OpenHashMap of twice the capacity (so it stays below its 7/8 load limit) plus a FIFO of the
// keys in insertion order. No iteration API, like OpenHashMap. Never allocates.
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/containers/ring_buffer.hpp"

#include <cstddef>

namespace fastmm {

template <class K, class V, std::size_t N>
class RecentMap {
 public:
  static constexpr std::size_t kCapacity = N;

  // Inserts or overwrites; a new key evicts the oldest one when the map is full. Returns true for a
  // new key.
  bool assign(const K& k, const V& v) noexcept {
    if (V* existing = map_.find(k)) {
      *existing = v;
      return false;
    }
    if (order_.full()) {
      K oldest{};
      order_.pop(oldest);
      map_.erase(oldest);
    }
    order_.push(k);
    static_cast<void>(map_.insert(k, v));
    return true;
  }
  [[nodiscard]] const V* find(const K& k) const noexcept { return map_.find(k); }
  [[nodiscard]] bool contains(const K& k) const noexcept { return map_.find(k) != nullptr; }
  [[nodiscard]] std::size_t size() const noexcept { return order_.size(); }
  void clear() noexcept {
    map_.clear();
    order_.clear();
  }

 private:
  OpenHashMap<K, V, 2 * N> map_;
  RingBuffer<K, N> order_;
};

}  // namespace fastmm
