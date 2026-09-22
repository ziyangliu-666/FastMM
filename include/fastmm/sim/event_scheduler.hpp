#pragma once
// EventScheduler<Payload, N>: fixed-capacity binary min-heap keyed by (fire_ts, insertion
// seq) so equal timestamps pop in insertion order (5.11). Payload is a small trivially
// copyable struct (the simulator uses it for order messages in flight). No allocation
// after construction.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/time.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace fastmm::sim {

template <class Payload, std::size_t N>
class EventScheduler {
  static_assert(std::is_trivially_copyable_v<Payload>);
  static_assert(N > 0 && N < UINT32_MAX);

 public:
  struct Entry {
    Timestamp fire_ts;
    std::uint64_t seq;
    Payload payload;
  };

  // The heap orders 24-byte keys; payloads stay in their slot from push to pop, so sifting
  // never moves a (192-byte, for the simulator's orders) payload.
  EventScheduler() : heap_(new Key[N]), payload_(new Payload[N]), free_(new std::uint32_t[N]) {
    clear();
  }
  EventScheduler(const EventScheduler&) = delete;
  EventScheduler& operator=(const EventScheduler&) = delete;

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] bool full() const noexcept { return size_ == N; }
  void clear() noexcept {
    size_ = 0;
    for (std::size_t i = 0; i < N; ++i) free_[i] = static_cast<std::uint32_t>(N - 1 - i);
  }

  // False when full.
  bool push(Timestamp fire_ts, const Payload& p) noexcept {
    if (FASTMM_UNLIKELY(size_ == N)) return false;
    const std::uint32_t slot = free_[N - 1 - size_];
    payload_[slot] = p;
    std::size_t i = size_++;
    const Key k{fire_ts, next_seq_++, slot};
    while (i > 0) {
      const std::size_t parent = (i - 1) / 2;
      if (!less(k, heap_[parent])) break;
      heap_[i] = heap_[parent];
      i = parent;
    }
    heap_[i] = k;
    return true;
  }
  // Timestamp::max() when empty.
  [[nodiscard]] Timestamp peek_ts() const noexcept {
    return size_ == 0 ? Timestamp::max() : heap_[0].fire_ts;
  }
  bool pop(Entry& out) noexcept {
    if (size_ == 0) return false;
    const Key top = heap_[0];
    out.fire_ts = top.fire_ts;
    out.seq = top.seq;
    out.payload = payload_[top.slot];
    --size_;
    free_[N - 1 - size_] = top.slot;
    if (size_ == 0) return true;
    const Key last = heap_[size_];
    std::size_t i = 0;
    for (;;) {
      const std::size_t l = 2 * i + 1;
      if (l >= size_) break;
      std::size_t m = l;
      if (l + 1 < size_ && less(heap_[l + 1], heap_[l])) m = l + 1;
      if (!less(heap_[m], last)) break;
      heap_[i] = heap_[m];
      i = m;
    }
    heap_[i] = last;
    return true;
  }

 private:
  struct Key {
    Timestamp fire_ts;
    std::uint64_t seq;
    std::uint32_t slot;
  };
  static bool less(const Key& a, const Key& b) noexcept {
    return a.fire_ts < b.fire_ts || (a.fire_ts == b.fire_ts && a.seq < b.seq);
  }
  std::unique_ptr<Key[]> heap_;
  std::unique_ptr<Payload[]> payload_;
  // Free payload slots: free_[0, N - size_), the next one at N - size_ - 1.
  std::unique_ptr<std::uint32_t[]> free_;
  std::size_t size_ = 0;
  std::uint64_t next_seq_ = 0;
};

}  // namespace fastmm::sim
