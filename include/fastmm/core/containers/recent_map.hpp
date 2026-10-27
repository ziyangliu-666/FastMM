#pragma once
// RecentMap<K, V, N>: the N most recently inserted keys. Inserting into a full map evicts the
// oldest key, so it never refuses and never grows: for lookups that only have to reach back a
// bounded distance (execution ids for deduplication, venue order ids for trade history).
//
// The keys and values sit in a ring in insertion order; an open-addressing index of twice the
// capacity (linear probing, backward-shift deletion) maps a key to its ring position. An index slot
// is 8 bytes, the ring position and the top 32 bits of the key's hash: a probe compares those bits
// and reads a key from the ring only when they match, and a deletion finds each entry's home slot
// from them without reading the ring. A lookup of a new key touches one index line, an insert
// that line and the ring's tail; the eviction it causes, the oldest key's slot, which is fetched
// ahead at the insert before. No iteration API, like OpenHashMap. Never allocates.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/hot_array.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace fastmm {

template <class K, class V, std::size_t N>
class RecentMap {
  static_assert(std::has_single_bit(N) && N >= 2 && N <= (std::size_t{1} << 31));
  static_assert(std::is_trivially_copyable_v<K> && std::is_trivially_copyable_v<V>);
  static constexpr std::size_t kSlots = 2 * N;
  static constexpr int kSlotBits = std::countr_zero(kSlots);

 public:
  static constexpr std::size_t kCapacity = N;

  // Construction happens at startup only; failing to allocate is fatal by design.
  // NOLINTNEXTLINE(bugprone-unhandled-exception-at-new)
  RecentMap() noexcept
      : index_(make_hot_array<std::uint64_t>(kSlots)),
        keys_(make_hot_array<K>(N)),
        values_(make_hot_array<V>(N)) {}
  RecentMap(const RecentMap&) = delete;
  RecentMap& operator=(const RecentMap&) = delete;

  // Inserts or overwrites; a new key evicts the oldest one when the map is full. Returns true for a
  // new key.
  bool assign(const K& k, const V& v) noexcept {
    const std::uint64_t h = hash(k);
    if (V* existing = find(k, h)) {
      *existing = v;
      return false;
    }
    if (tail_ - head_ == N) {
      const std::size_t oldest = head_ & (N - 1);
      unindex(hash(keys_[oldest]), oldest);
      ++head_;
    }
    const std::size_t pos = tail_ & (N - 1);
    keys_[pos] = k;
    values_[pos] = v;
    ++tail_;
    std::size_t i = home(h);
    while (index_[i] != 0) i = (i + 1) & (kSlots - 1);
    index_[i] = ((h >> 32U) << 32U) | (pos + 1);
    // The next insert evicts the key that is the oldest now: fetch its slot while it is known.
    if (tail_ - head_ == N) __builtin_prefetch(&index_[home(hash(keys_[head_ & (N - 1)]))], 1);
    return true;
  }
  [[nodiscard]] const V* find(const K& k) const noexcept {
    return const_cast<RecentMap*>(this)->find(k, hash(k));
  }
  [[nodiscard]] bool contains(const K& k) const noexcept { return find(k) != nullptr; }
  [[nodiscard]] std::size_t size() const noexcept { return tail_ - head_; }
  void clear() noexcept {
    for (std::size_t i = 0; i < kSlots; ++i) index_[i] = 0;
    head_ = tail_ = 0;
  }

 private:
  [[nodiscard]] static std::uint64_t hash(const K& k) noexcept {
    return detail::KeyRep<K>::to_u64(k) * 0x9E3779B97F4A7C15ULL;
  }
  // The slot a hash probes first: its top bits, which the slot keeps.
  [[nodiscard]] static std::size_t home(std::uint64_t h) noexcept { return h >> (64 - kSlotBits); }
  [[nodiscard]] FASTMM_FORCE_INLINE V* find(const K& k, std::uint64_t h) noexcept {
    const std::uint64_t top = h >> 32U;
    for (std::size_t i = home(h);; i = (i + 1) & (kSlots - 1)) {
      const std::uint64_t s = index_[i];
      if (s == 0) return nullptr;
      if ((s >> 32U) != top) continue;
      const std::size_t pos = (s & 0xFFFF'FFFFU) - 1;
      if (keys_[pos] == k) return &values_[pos];
    }
  }
  // Removes the slot of ring position `pos`, whose key hashes to `h`, and closes the gap.
  void unindex(std::uint64_t h, std::size_t pos) noexcept {
    std::size_t hole = home(h);
    while ((index_[hole] & 0xFFFF'FFFFU) != pos + 1) hole = (hole + 1) & (kSlots - 1);
    for (std::size_t j = (hole + 1) & (kSlots - 1); index_[j] != 0; j = (j + 1) & (kSlots - 1)) {
      const std::size_t at = index_[j] >> (64 - kSlotBits);
      // Slot j may move into `hole` iff its home is not in the cyclic range (hole, j].
      const bool in_range = hole <= j ? (at > hole && at <= j) : (at > hole || at <= j);
      if (!in_range) {
        index_[hole] = index_[j];
        hole = j;
      }
    }
    index_[hole] = 0;
  }

  HotArray<std::uint64_t> index_;  // ((hash >> 32) << 32) | (ring position + 1); 0 is empty
  HotArray<K> keys_;
  HotArray<V> values_;
  std::uint64_t head_ = 0;  // keys inserted and evicted, as ring positions
  std::uint64_t tail_ = 0;
};

}  // namespace fastmm
