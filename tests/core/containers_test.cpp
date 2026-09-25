#include "test_support.hpp"

#include "fastmm/core/containers/counter_key_map.hpp"
#include "fastmm/core/containers/flat_map.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/containers/pool.hpp"
#include "fastmm/core/containers/recent_map.hpp"
#include "fastmm/core/containers/ring_buffer.hpp"
#include "fastmm/core/containers/static_vector.hpp"
#include "fastmm/core/fixed_string.hpp"
#include "fastmm/core/strong_id.hpp"

#include <algorithm>
#include <random>
#include <unordered_map>
#include <vector>

using namespace fastmm;

TEST_CASE("core.static_vector: push/insert/erase/memmove semantics") {
  StaticVector<int, 4> v;
  CHECK(v.empty());
  CHECK(v.push_back(1));
  CHECK(v.push_back(3));
  CHECK(v.insert_at(1, 2));  // 1 2 3
  CHECK(v.insert_at(3, 4));  // 1 2 3 4
  CHECK_FALSE(v.push_back(5));
  CHECK_FALSE(v.insert_at(0, 0));
  CHECK(v.full());
  CHECK(v.size() == 4);
  CHECK(v[0] == 1);
  CHECK(v[3] == 4);
  v.erase_at(1);  // 1 3 4
  CHECK(v.size() == 3);
  CHECK(v[1] == 3);
  v.erase_front();  // 3 4
  CHECK(v.front() == 3);
  CHECK(v.back() == 4);
  v.pop_back();
  CHECK(v.size() == 1);
  int src[3] = {7, 8, 9};
  CHECK(v.assign(src, 3));
  CHECK(v[2] == 9);
  int big[5] = {1, 2, 3, 4, 5};
  CHECK_FALSE(v.assign(big, 5));
  CHECK(v.size() == 4);
  v.resize_down(2);
  CHECK(v.size() == 2);
  int sum = 0;
  for (int x : v) sum += x;
  CHECK(sum == 3);
  v.clear();
  CHECK(v.empty());
}

TEST_CASE("core.flat_map: sorted insert/find/erase") {
  FlatMap<FixedString<16>, std::uint32_t, 8> m;
  CHECK(m.insert("btcusdt", 1).second);
  CHECK(m.insert("ethusdt", 2).second);
  CHECK(m.insert("adausdt", 3).second);
  CHECK_FALSE(m.insert("btcusdt", 9).second);
  CHECK(*m.find("btcusdt") == 1);
  CHECK(*m.find("adausdt") == 3);
  CHECK(m.find("xrpusdt") == nullptr);
  CHECK(m.size() == 3);
  // sorted iteration
  std::vector<std::string> keys;
  for (const auto& e : m) keys.emplace_back(e.key.view());
  CHECK(std::is_sorted(keys.begin(), keys.end()));
  CHECK(m.erase("ethusdt"));
  CHECK_FALSE(m.erase("ethusdt"));
  CHECK(m.size() == 2);
  CHECK(m.assign("adausdt", 33) != nullptr);
  CHECK(*m.find("adausdt") == 33);
  for (int i = 0; i < 6; ++i)
    m.insert(FixedString<16>(std::string(1, static_cast<char>('a' + i))), 0);
  CHECK(m.size() == 8);
  CHECK(m.insert("zzz", 0).first == nullptr);
}

TEST_CASE("core.open_hash_map: randomized 1M ops vs std::unordered_map") {
  constexpr std::size_t kN = 1 << 16;
  OpenHashMap<std::uint64_t, std::uint32_t, kN> m;
  std::unordered_map<std::uint64_t, std::uint32_t> ref;
  std::mt19937_64 rng(42);
  std::uniform_int_distribution<std::uint64_t> key(1, 60'000);  // dense enough to collide
  std::uniform_int_distribution<int> op(0, 9);
  std::size_t mismatches = 0;
  for (int i = 0; i < 1'000'000; ++i) {
    const std::uint64_t k = key(rng);
    const int o = op(rng);
    if (o < 5) {  // insert
      const auto v = static_cast<std::uint32_t>(i);
      auto [p, ins] = m.insert(k, v);
      const auto [rit, rins] = ref.emplace(k, v);
      if (p == nullptr) {  // full
        if (ref.size() - (rins ? 1 : 0) < OpenHashMap<std::uint64_t, std::uint32_t, kN>::kMaxSize)
          ++mismatches;
        if (rins) ref.erase(rit);
      } else if (ins != rins || *p != rit->second) {
        ++mismatches;
      }
    } else if (o < 8) {  // find
      const auto* p = m.find(k);
      const auto it = ref.find(k);
      if ((p == nullptr) != (it == ref.end()))
        ++mismatches;
      else if (p != nullptr && *p != it->second)
        ++mismatches;
    } else {  // erase
      if (m.erase(k) != (ref.erase(k) == 1)) ++mismatches;
    }
    if (m.size() != ref.size()) ++mismatches;
  }
  CHECK(mismatches == 0);
  // final full comparison
  for (const auto& [k, v] : ref) {
    const auto* p = m.find(k);
    REQUIRE(p != nullptr);
    CHECK(*p == v);
  }
  m.clear();
  CHECK(m.empty());
  CHECK(m.find(1) == nullptr);
}

TEST_CASE("core.counter_key_map: direct slots, overflow for colliding keys, vs a reference") {
  CounterKeyMap<std::uint32_t, 64, 16> m;  // overflow holds 14
  std::unordered_map<std::uint64_t, std::uint32_t> ref;
  CHECK_FALSE(m.insert(0, 1));  // 0 marks an empty slot
  CHECK(m.find(0) == nullptr);
  // Keys 64 apart share a slot: the first takes it, the next 14 go to the overflow table.
  for (std::uint64_t k = 5; k < 5 + 64 * 15; k += 64) {
    REQUIRE(m.insert(k, static_cast<std::uint32_t>(k)));
    ref.emplace(k, static_cast<std::uint32_t>(k));
  }
  CHECK_FALSE(m.insert(5 + 64 * 15, 0));  // slot and overflow taken
  CHECK(m.size() == 15);
  CHECK(m.erase(5));  // the slot's owner leaves; the others stay findable
  ref.erase(5);
  CHECK(m.find(5) == nullptr);
  for (const auto& [k, v] : ref) {
    REQUIRE(m.find(k) != nullptr);
    CHECK(*m.find(k) == v);
  }
  // A counter's keys: randomized erase order, then the next keys reuse the freed slots.
  std::mt19937_64 rng(7);
  std::uint64_t next = 1000;
  for (int i = 0; i < 100'000; ++i) {
    if (ref.size() < 40 && rng() % 2 == 0) {
      const std::uint64_t k = next++;
      if (m.insert(k, static_cast<std::uint32_t>(i))) ref.emplace(k, static_cast<std::uint32_t>(i));
    } else if (!ref.empty()) {
      auto it = ref.begin();
      std::advance(it, static_cast<std::ptrdiff_t>(rng() % ref.size()));
      REQUIRE(m.erase(it->first));
      ref.erase(it);
    }
    REQUIRE(m.size() == ref.size());
  }
  for (const auto& [k, v] : ref) {
    REQUIRE(m.find(k) != nullptr);
    CHECK(*m.find(k) == v);
  }
}

TEST_CASE("core.open_hash_map: load cap and StrongId keys") {
  OpenHashMap<ClientOrderId, std::uint32_t, 16> m;
  for (std::uint64_t i = 1; i <= 14; ++i) CHECK(m.insert(ClientOrderId{i}, 0).first != nullptr);
  CHECK(m.insert(ClientOrderId{99}, 0).first == nullptr);  // 14 == 16 - 2
  CHECK(m.size() == 14);
  CHECK(m.contains(ClientOrderId{7}));
  CHECK(m.erase(ClientOrderId{7}));
  CHECK_FALSE(m.contains(ClientOrderId{7}));
  CHECK(m.insert(ClientOrderId{99}, 5).second);
  CHECK(*m.find(ClientOrderId{99}) == 5);
  CHECK(m.assign(ClientOrderId{99}, 6) != nullptr);
  CHECK(*m.find(ClientOrderId{99}) == 6);
}

TEST_CASE("core.pool: allocate/free/exhaustion/handle order") {
  struct Obj {
    int x;
    int y;
  };
  Pool<Obj, 4> p;
  p.warm_up();
  auto a = p.allocate(Obj{1, 1});
  auto b = p.allocate(Obj{2, 2});
  auto c = p.allocate();
  auto d = p.allocate();
  CHECK(a.valid());
  CHECK(d.valid());
  CHECK(p.full());
  CHECK_FALSE(p.allocate().valid());  // exhausted -> null handle
  CHECK(p.size() == 4);
  CHECK(p.get(a).x == 1);
  CHECK(p.handle_of(p.get(b)) == b);
  p.free(b);
  CHECK_FALSE(p.is_live(b));
  CHECK(p.is_live(a));
  auto e = p.allocate(Obj{5, 5});
  CHECK(e == b);  // LIFO reuse
  p.free(c);
  p.free(a);
  // for_each visits ascending handle order regardless of alloc/free order
  std::vector<std::uint32_t> order;
  p.for_each([&](Handle<Obj> h, Obj&) { order.push_back(h.idx); });
  CHECK(order == std::vector<std::uint32_t>{b.idx, d.idx});
  p.reset();
  CHECK(p.size() == 0);
  CHECK(p.allocate().idx == 0);
}

TEST_CASE("core.ring_buffer: overwrite and fifo") {
  RingBuffer<int, 4> r;
  for (int i = 0; i < 6; ++i) r.push(i);
  CHECK(r.full());
  CHECK(r.size() == 4);
  CHECK(r.front() == 2);
  CHECK(r.back() == 5);
  CHECK(r[0] == 2);
  CHECK(r[3] == 5);
  CHECK_FALSE(r.try_push(9));
  int v = 0;
  CHECK(r.pop(v));
  CHECK(v == 2);
  CHECK(r.try_push(9));
  CHECK(*r.find_if([](int x) { return x == 9; }) == 9);
  CHECK(r.find_if([](int x) { return x == 2; }) == nullptr);
  r.clear();
  CHECK(r.empty());
  CHECK_FALSE(r.pop(v));
}

TEST_CASE("core.recent_map: a full map evicts the oldest key, never refuses") {
  RecentMap<std::uint64_t, int, 4> m;
  for (std::uint64_t k = 1; k <= 4; ++k) CHECK(m.assign(k, static_cast<int>(k)));
  CHECK_FALSE(m.assign(2, 20));  // an overwrite is not a new key and moves nothing
  CHECK(*m.find(2) == 20);
  CHECK(m.assign(5, 5));  // evicts 1, the oldest
  CHECK_FALSE(m.contains(1));
  CHECK(m.contains(2));
  CHECK(m.size() == 4);
  for (std::uint64_t k = 6; k < 10'000; ++k) CHECK(m.assign(k, 0));
  CHECK(m.size() == 4);
  CHECK(m.contains(9'999));
  CHECK_FALSE(m.contains(9'995));
}
