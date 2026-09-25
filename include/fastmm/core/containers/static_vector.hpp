#pragma once
// StaticVector<T, N>: fixed-capacity vector with inline storage for trivially copyable T.
// push_back returns false when full (never throws). insert_at/erase_at use memmove so
// near-end edits in the L2 book cost a handful of cycles.
#include "fastmm/core/config_macros.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace fastmm {

template <class T, std::size_t N>
class StaticVector {
  static_assert(std::is_trivially_copyable_v<T>, "StaticVector requires trivially copyable T");
  static_assert(N > 0);

 public:
  using value_type = T;
  using size_type = std::uint32_t;
  using iterator = T*;
  using const_iterator = const T*;

  constexpr StaticVector() noexcept = default;

  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }
  [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] constexpr bool full() const noexcept { return size_ == N; }
  constexpr void clear() noexcept { size_ = 0; }

  [[nodiscard]] constexpr T* data() noexcept { return data_; }
  [[nodiscard]] constexpr const T* data() const noexcept { return data_; }
  constexpr T& operator[](std::size_t i) noexcept {
    FASTMM_ASSERT(i < size_);
    return data_[i];
  }
  constexpr const T& operator[](std::size_t i) const noexcept {
    FASTMM_ASSERT(i < size_);
    return data_[i];
  }
  constexpr T& front() noexcept { return (*this)[0]; }
  constexpr const T& front() const noexcept { return (*this)[0]; }
  constexpr T& back() noexcept { return (*this)[size_ - 1]; }
  constexpr const T& back() const noexcept { return (*this)[size_ - 1]; }
  constexpr iterator begin() noexcept { return data_; }
  constexpr iterator end() noexcept { return data_ + size_; }
  constexpr const_iterator begin() const noexcept { return data_; }
  constexpr const_iterator end() const noexcept { return data_ + size_; }

  constexpr bool push_back(const T& v) noexcept {
    if (FASTMM_UNLIKELY(size_ == N)) return false;
    data_[size_++] = v;
    return true;
  }
  constexpr void pop_back() noexcept {
    FASTMM_ASSERT(size_ > 0);
    --size_;
  }
  // Drops elements beyond n (n <= size()).
  constexpr void resize_down(std::size_t n) noexcept {
    FASTMM_ASSERT(n <= size_);
    size_ = static_cast<size_type>(n);
  }
  // Grows to n with unspecified contents (n <= N); caller fills.
  constexpr bool resize_uninitialized(std::size_t n) noexcept {
    if (n > N) return false;
    size_ = static_cast<size_type>(n);
    return true;
  }

  // Inserts v at index i (0 <= i <= size), shifting [i, size) up by one. False if full.
  bool insert_at(std::size_t i, const T& v) noexcept {
    FASTMM_ASSERT(i <= size_);
    if (FASTMM_UNLIKELY(size_ == N || i > size_)) return false;
    if (size_ > N) FASTMM_UNREACHABLE();  // range hint for the optimiser (memmove bound)
    if (i < size_) std::memmove(data_ + i + 1, data_ + i, (size_ - i) * sizeof(T));
    data_[i] = v;
    ++size_;
    return true;
  }
  void erase_at(std::size_t i) noexcept {
    FASTMM_ASSERT(i < size_);
    if (FASTMM_UNLIKELY(i >= size_)) return;
    if (size_ > N) FASTMM_UNREACHABLE();
    // Shifting down: std::copy is defined for this overlap and is a memmove for trivial types.
    std::copy(data_ + i + 1, data_ + size_, data_ + i);
    --size_;
  }
  // Removes the first element (shifts everything down) - O(n); used when the book is full.
  void erase_front() noexcept { erase_at(0); }

  // Bulk assign from a range of trivially copyable T. Truncates to N; returns false then.
  bool assign(const T* src, std::size_t n) noexcept {
    const bool ok = n <= N;
    if (!ok) n = N;
    if (n > 0) std::memcpy(data_, src, n * sizeof(T));
    size_ = static_cast<size_type>(n);
    return ok;
  }

 private:
  size_type size_ = 0;
  T data_[N];
};

}  // namespace fastmm
