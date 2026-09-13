#include "fastmm/core/arena.hpp"

#include "test_support.hpp"

using namespace fastmm;

TEST_CASE("core.arena: bump allocation, alignment, exhaustion") {
  Arena a(1 << 20);
  CHECK(a.capacity() >= (1U << 20));
  CHECK(a.used() == 0);
  void* p = a.allocate(100);
  REQUIRE(p != nullptr);
  CHECK(reinterpret_cast<std::uintptr_t>(p) % kCacheLine == 0);
  void* q = a.allocate(8, 4096);
  REQUIRE(q != nullptr);
  CHECK(reinterpret_cast<std::uintptr_t>(q) % 4096 == 0);
  CHECK(a.used() >= 4096 + 8);
  struct Big {
    alignas(64) char buf[256];
  };
  Big* b = a.create<Big>();
  REQUIRE(b != nullptr);
  CHECK(reinterpret_cast<std::uintptr_t>(b) % 64 == 0);
  int* arr = a.create_array<int>(1000);
  REQUIRE(arr != nullptr);
  CHECK(arr[999] == 0);
  CHECK(a.allocate(a.remaining() + 1, 1) == nullptr);
  CHECK(a.allocate(a.remaining(), 1) != nullptr);
  CHECK(a.remaining() == 0);
  Arena moved(std::move(a));
  CHECK(moved.remaining() == 0);
  CHECK(moved.base() != nullptr);
}
