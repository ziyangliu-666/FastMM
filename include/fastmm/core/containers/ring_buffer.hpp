#pragma once
// RingBuffer<T, N>: single-threaded circular FIFO with power-of-two capacity.
// push() overwrites the oldest element when full (the "recently terminal" use case);
// try_push() refuses instead.
#include "fastmm/core/config_macros.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace fastmm {

template <class T, std::size_t N>
class RingBuffer {
  static_assert(std::has_single_bit(N));
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  [[nodiscard]] std::size_t size() const noexcept { return tail_ - head_; }
  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }
  [[nodiscard]] bool empty() const noexcept { return head_ == tail_; }
  [[nodiscard]] bool full() const noexcept { return size() == N; }
  void clear() noexcept { head_ = tail_ = 0; }

  void push(const T& v) noexcept {
    if (full()) ++head_;
    buf_[tail_ & (N - 1)] = v;
    ++tail_;
  }
  bool try_push(const T& v) noexcept {
    if (full()) return false;
    buf_[tail_ & (N - 1)] = v;
    ++tail_;
    return true;
  }
  bool pop(T& out) noexcept {
    if (empty()) return false;
    out = buf_[head_ & (N - 1)];
    ++head_;
    return true;
  }
  void pop() noexcept {
    FASTMM_ASSERT(!empty());
    ++head_;
  }
  [[nodiscard]] T& front() noexcept { return buf_[head_ & (N - 1)]; }
  [[nodiscard]] const T& front() const noexcept { return buf_[head_ & (N - 1)]; }
  [[nodiscard]] T& back() noexcept { return buf_[(tail_ - 1) & (N - 1)]; }
  // i == 0 is the oldest element.
  [[nodiscard]] const T& operator[](std::size_t i) const noexcept {
    return buf_[(head_ + i) & (N - 1)];
  }
  [[nodiscard]] T& operator[](std::size_t i) noexcept { return buf_[(head_ + i) & (N - 1)]; }

  // Linear scan (newest first); returns nullptr if absent.
  template <class Pred>
  [[nodiscard]] const T* find_if(Pred&& p) const noexcept {
    for (std::size_t i = size(); i > 0; --i) {
      const T& e = (*this)[i - 1];
      if (p(e)) return &e;
    }
    return nullptr;
  }

 private:
  std::uint64_t head_ = 0;
  std::uint64_t tail_ = 0;
  T buf_[N];
};

}  // namespace fastmm
