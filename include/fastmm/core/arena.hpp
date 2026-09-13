#pragma once
// Arena: one mmap'd, pre-faulted, hugepage-advised region carved out with bump allocation
// at startup. Objects are never individually freed; the whole region is unmapped on
// destruction. Only trivially destructible types may be placed here.
#include "fastmm/core/config_macros.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace fastmm {

class Arena {
 public:
  // Throws std::bad_alloc if mmap fails (startup only).
  explicit Arena(std::size_t bytes);
  ~Arena();
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&& o) noexcept;
  Arena& operator=(Arena&&) = delete;

  // nullptr when exhausted. align must be a power of two.
  [[nodiscard]] void* allocate(std::size_t bytes, std::size_t align = kCacheLine) noexcept;

  template <class T, class... Args>
  [[nodiscard]] T* create(Args&&... args) noexcept {
    static_assert(std::is_trivially_destructible_v<T>, "arena objects are never destroyed");
    void* p = allocate(sizeof(T), alignof(T) > kCacheLine ? alignof(T) : kCacheLine);
    if (p == nullptr) return nullptr;
    return ::new (p) T(std::forward<Args>(args)...);
  }
  template <class T>
  [[nodiscard]] T* create_array(std::size_t n) noexcept {
    static_assert(std::is_trivially_destructible_v<T>);
    void* p = allocate(sizeof(T) * n, alignof(T) > kCacheLine ? alignof(T) : kCacheLine);
    if (p == nullptr) return nullptr;
    return ::new (p) T[n]();
  }

  [[nodiscard]] std::size_t used() const noexcept { return used_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return capacity_ - used_; }
  [[nodiscard]] bool huge_pages_advised() const noexcept { return huge_; }
  [[nodiscard]] std::byte* base() const noexcept { return base_; }

 private:
  std::byte* base_ = nullptr;
  std::size_t capacity_ = 0;
  std::size_t used_ = 0;
  bool huge_ = false;
};

}  // namespace fastmm
