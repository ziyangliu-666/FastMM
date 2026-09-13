#include "test_support.hpp"

#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/spsc_ring.hpp"

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

using namespace fastmm;

TEST_CASE("core.spsc_ring: basic and reserve/commit") {
  auto ring = std::make_unique<SpscRing<int, 4>>();
  CHECK(ring->front() == nullptr);
  CHECK(ring->try_push(1));
  CHECK(ring->try_push(2));
  CHECK(ring->try_push(3));
  CHECK(ring->try_push(4));
  CHECK_FALSE(ring->try_push(5));
  CHECK(ring->size_approx() == 4);
  int v = 0;
  CHECK(ring->try_pop(v));
  CHECK(v == 1);
  int* slot = ring->try_reserve();
  REQUIRE(slot != nullptr);
  *slot = 9;
  ring->commit();
  for (int expect : {2, 3, 4, 9}) {
    CHECK(ring->try_pop(v));
    CHECK(v == expect);
  }
  CHECK_FALSE(ring->try_pop(v));
}

TEST_CASE("core.spsc_ring: two threads, 10M items in order") {
  constexpr std::uint64_t kItems = 10'000'000;
  auto ring = std::make_unique<SpscRing<std::uint64_t, 1024>>();
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kItems; ++i) {
      while (!ring->try_push(i)) {
      }
    }
  });
  std::uint64_t expect = 0;
  std::uint64_t bad = 0;
  while (expect < kItems) {
    std::uint64_t v = 0;
    if (!ring->try_pop(v)) continue;
    if (v != expect) ++bad;
    ++expect;
  }
  producer.join();
  CHECK(bad == 0);
  CHECK(ring->empty_approx());
}

namespace {
struct TestMsg {
  std::uint32_t len;
  std::uint8_t type;
  std::uint8_t pad[3];
  std::uint64_t seq;
  std::uint8_t payload[48];
};
static_assert(sizeof(TestMsg) == 64);
}  // namespace

TEST_CASE("core.msg_ring: reserve/commit/peek/release with wrap padding") {
  MsgRing ring(256);  // 4 granules
  auto push = [&](std::uint32_t len, std::uint64_t seq) {
    std::byte* p = ring.try_reserve(len);
    if (p == nullptr) return false;
    auto* m = reinterpret_cast<TestMsg*>(p);
    m->len = (len + 63) & ~63U;
    m->type = 1;
    m->seq = seq;
    ring.commit();
    return true;
  };
  auto pop = [&]() -> std::int64_t {
    const std::byte* p = ring.try_peek();
    if (p == nullptr) return -1;
    const auto seq = reinterpret_cast<const TestMsg*>(p)->seq;
    ring.release();
    return static_cast<std::int64_t>(seq);
  };
  CHECK(pop() == -1);
  CHECK(push(64, 1));
  CHECK(push(128, 2));        // 192 used
  CHECK_FALSE(push(128, 3));  // only 64 free
  CHECK(push(64, 3));         // full 256
  CHECK_FALSE(push(1, 4));
  CHECK(pop() == 1);
  CHECK(pop() == 2);
  // now head=192; a 128-byte message must wrap: needs 64 pad + 128 -> free = 192 OK
  CHECK(push(128, 4));
  CHECK(pop() == 3);
  CHECK(pop() == 4);  // padding skipped transparently
  CHECK(pop() == -1);
  CHECK(ring.empty_approx());
  // odd lengths round up to 64
  CHECK(push(65, 5));
  CHECK(ring.bytes_used_approx() % 64 == 0);
  CHECK(pop() == 5);
}

TEST_CASE("core.msg_ring: arena-backed construction") {
  Arena arena(1 << 20);
  MsgRing ring(arena, 4096);
  CHECK(ring.capacity() == 4096);
  TestMsg m{};
  m.len = 64;
  m.type = 2;
  m.seq = 77;
  CHECK(ring.try_push(&m, 64));
  const auto* p = reinterpret_cast<const TestMsg*>(ring.try_peek());
  REQUIRE(p != nullptr);
  CHECK(p->seq == 77);
  ring.release();
}

TEST_CASE("core.msg_ring: two-thread stress with variable sizes stays ordered") {
  constexpr std::uint64_t kMsgs = 2'000'000;
  MsgRing ring(1 << 16);
  std::thread producer([&] {
    std::uint64_t x = 12345;
    for (std::uint64_t i = 0; i < kMsgs; ++i) {
      x = x * 6364136223846793005ULL + 1442695040888963407ULL;
      const std::uint32_t len = 64U * (1U + static_cast<std::uint32_t>((x >> 33) % 8));  // 64..512
      std::byte* p = nullptr;
      while ((p = ring.try_reserve(len)) == nullptr) {
      }
      auto* m = reinterpret_cast<TestMsg*>(p);
      m->len = len;
      m->type = 1;
      m->seq = i;
      std::memset(p + sizeof(TestMsg), static_cast<int>(i & 0xFF), len - sizeof(TestMsg));
      ring.commit();
    }
  });
  std::uint64_t expect = 0;
  std::uint64_t bad = 0;
  while (expect < kMsgs) {
    const std::byte* p = ring.try_peek();
    if (p == nullptr) continue;
    const auto* m = reinterpret_cast<const TestMsg*>(p);
    if (m->seq != expect) ++bad;
    if (m->len > sizeof(TestMsg) && static_cast<unsigned>(p[sizeof(TestMsg)]) != (expect & 0xFF))
      ++bad;
    ring.release();
    ++expect;
  }
  producer.join();
  CHECK(bad == 0);
}
