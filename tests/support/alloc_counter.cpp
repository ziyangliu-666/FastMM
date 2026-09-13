#include "alloc_counter.hpp"

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <new>

namespace {
thread_local fastmm::test::AllocStats t_stats;
thread_local bool t_trap = false;

void* counted_alloc(std::size_t n, std::size_t align) {
  if (t_trap) {
    std::fputs("FATAL: heap allocation inside NoAllocScope(trap)\n", stderr);
    std::abort();
  }
  ++t_stats.allocs;
  t_stats.bytes += n;
  void* p = align > alignof(std::max_align_t)
                ? std::aligned_alloc(align, (n + align - 1) / align * align)
                : std::malloc(n == 0 ? 1 : n);
  if (!p) throw std::bad_alloc();
  return p;
}
void counted_free(void* p) noexcept {
  if (p) ++t_stats.frees;
  std::free(p);
}
}  // namespace

void* operator new(std::size_t n) {
  return counted_alloc(n, alignof(std::max_align_t));
}
void* operator new[](std::size_t n) {
  return counted_alloc(n, alignof(std::max_align_t));
}
void* operator new(std::size_t n, std::align_val_t a) {
  return counted_alloc(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a) {
  return counted_alloc(n, static_cast<std::size_t>(a));
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  try {
    return counted_alloc(n, alignof(std::max_align_t));
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  try {
    return counted_alloc(n, alignof(std::max_align_t));
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void* p) noexcept {
  counted_free(p);
}
void operator delete[](void* p) noexcept {
  counted_free(p);
}
void operator delete(void* p, std::size_t) noexcept {
  counted_free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
  counted_free(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
  counted_free(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
  counted_free(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
  counted_free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
  counted_free(p);
}

namespace fastmm::test {

AllocStats alloc_stats() noexcept {
  return t_stats;
}
void set_alloc_trap(bool enabled) noexcept {
  t_trap = enabled;
}

NoAllocScope::NoAllocScope(bool trap) noexcept : start_(t_stats), trap_(trap) {
  if (trap_) t_trap = true;
}
NoAllocScope::~NoAllocScope() {
  if (trap_) t_trap = false;
  const auto delta = t_stats.allocs - start_.allocs;
  CHECK_MESSAGE(delta == 0, "hot path allocated " << delta << " time(s)");
}
std::uint64_t NoAllocScope::allocations_so_far() const noexcept {
  return t_stats.allocs - start_.allocs;
}

}  // namespace fastmm::test
