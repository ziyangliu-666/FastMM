#include <memory>
#include <vector>

#include "alloc_counter.hpp"
#include "test_support.hpp"

namespace {
// Prevent the optimizer from eliding an unused heap allocation (clang does this at -O3).
inline void escape(void* p) { asm volatile("" : : "g"(p) : "memory"); }
}  // namespace

TEST_CASE("hotpath.smoke: alloc counter sees heap allocations") {
  const auto before = fastmm::test::alloc_stats().allocs;
  {
    std::vector<int> v(1024);
    escape(v.data());
  }
  CHECK(fastmm::test::alloc_stats().allocs > before);
  {
    fastmm::test::NoAllocScope guard;
    int stack_only[8] = {};
    stack_only[3] = 1;
    escape(stack_only);
    CHECK(stack_only[3] == 1);
  }
}
