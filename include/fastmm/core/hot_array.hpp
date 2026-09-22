#pragma once
// HotArray<T>: a fixed-size array for hot-path tables, value-initialised and resident from the
// start. `new T[n]` leaves a trivially constructible T untouched, so every page is faulted in
// by the first write that lands on it: a hash table keyed by a spread-out hash pays one page
// fault (1 to 3 us, more under a hypervisor) per new key until every page has been hit.
// make_hot_array() writes the whole array at construction instead. Arrays of kHugeMin bytes
// or more are mmap'd at a 2 MiB boundary with MADV_HUGEPAGE, which also removes most dTLB
// misses of random probes (THP "madvise" mode is enough).
#include "fastmm/core/config_macros.hpp"

#include <cstddef>
#include <memory>
#include <type_traits>

namespace fastmm {

namespace detail {
// Zeroed, resident memory. `mapped` is set to the mapping length (0: operator new).
// Throws std::bad_alloc on failure (startup only).
void* hot_alloc(std::size_t bytes, std::size_t& mapped);
void hot_free(void* p, std::size_t mapped) noexcept;
}  // namespace detail

struct HotArrayDeleter {
  std::size_t mapped = 0;
  void operator()(void* p) const noexcept { detail::hot_free(p, mapped); }
};

template <class T>
using HotArray = std::unique_ptr<T[], HotArrayDeleter>;

inline constexpr std::size_t kHotArrayHugeMin = std::size_t{1} << 20;

template <class T>
[[nodiscard]] HotArray<T> make_hot_array(std::size_t n) {
  static_assert(std::is_trivially_destructible_v<T>, "HotArray never runs destructors");
  static_assert(alignof(T) <= kCacheLine);
  std::size_t mapped = 0;
  void* p = detail::hot_alloc(n * sizeof(T), mapped);
  T* a = static_cast<T*>(p);
  // Memory is already zero; this runs default member initialisers, if any.
  if constexpr (!std::is_trivially_default_constructible_v<T>) {
    std::uninitialized_value_construct_n(a, n);
  }
  return HotArray<T>(a, HotArrayDeleter{mapped});
}

}  // namespace fastmm
