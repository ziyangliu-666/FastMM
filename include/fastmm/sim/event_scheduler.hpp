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
  static_assert(N > 0);

 public:
  struct Entry {
    Timestamp fire_ts;
    std::uint64_t seq;
    Payload payload;
  };

  EventScheduler() : heap_(new Entry[N]) {}
  EventScheduler(const EventScheduler&) = delete;
  EventScheduler& operator=(const EventScheduler&) = delete;

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] bool full() const noexcept { return size_ == N; }
  void clear() noexcept { size_ = 0; }

  // False when full.
  bool push(Timestamp fire_ts, const Payload& p) noexcept {
    if (FASTMM_UNLIKELY(size_ == N)) return false;
    std::size_t i = size_++;
    heap_[i] = Entry{fire_ts, next_seq_++, p};
    while (i > 0) {
      const std::size_t parent = (i - 1) / 2;
      if (!less(heap_[i], heap_[parent])) break;
      std::swap(heap_[i], heap_[parent]);
      i = parent;
    }
    return true;
  }
  // Timestamp::max() when empty.
  [[nodiscard]] Timestamp peek_ts() const noexcept {
    return size_ == 0 ? Timestamp::max() : heap_[0].fire_ts;
  }
  [[nodiscard]] const Entry& peek() const noexcept {
    FASTMM_ASSERT(size_ > 0);
    return heap_[0];
  }
  bool pop(Entry& out) noexcept {
    if (size_ == 0) return false;
    out = heap_[0];
    --size_;
    if (size_ == 0) return true;
    heap_[0] = heap_[size_];
    std::size_t i = 0;
    for (;;) {
      const std::size_t l = 2 * i + 1;
      const std::size_t r = l + 1;
      std::size_t m = i;
      if (l < size_ && less(heap_[l], heap_[m])) m = l;
      if (r < size_ && less(heap_[r], heap_[m])) m = r;
      if (m == i) break;
      std::swap(heap_[i], heap_[m]);
      i = m;
    }
    return true;
  }

 private:
  static bool less(const Entry& a, const Entry& b) noexcept {
    return a.fire_ts < b.fire_ts || (a.fire_ts == b.fire_ts && a.seq < b.seq);
  }
  std::unique_ptr<Entry[]> heap_;
  std::size_t size_ = 0;
  std::uint64_t next_seq_ = 0;
};

}  // namespace fastmm::sim
