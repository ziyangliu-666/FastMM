#pragma once
// Global operator new/delete override with per-thread counting. Linked ONLY into
// fastmm_hotpath_tests (tests/CMakeLists.txt). NoAllocScope asserts zero allocations.
#include <cstddef>
#include <cstdint>

namespace fastmm::test {

struct AllocStats {
  std::uint64_t allocs = 0;
  std::uint64_t frees = 0;
  std::uint64_t bytes = 0;
};

AllocStats alloc_stats() noexcept;           // for the calling thread
void set_alloc_trap(bool enabled) noexcept;  // abort() on the next allocation in this thread

class NoAllocScope {
 public:
  explicit NoAllocScope(bool trap = false) noexcept;
  ~NoAllocScope();
  NoAllocScope(const NoAllocScope&) = delete;
  NoAllocScope& operator=(const NoAllocScope&) = delete;
  std::uint64_t allocations_so_far() const noexcept;

 private:
  AllocStats start_;
  bool trap_;
};

}  // namespace fastmm::test
