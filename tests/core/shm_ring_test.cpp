#include "fastmm/core/shm_ring.hpp"

#include "test_support.hpp"

#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/transport.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace fastmm;
using fastmm::test::tmp_dir;

namespace {

// A record in the ring protocol: 4-byte length, 1-byte type, then a sequence number and filler.
std::vector<std::byte> record(std::uint32_t len, std::uint64_t seq) {
  std::vector<std::byte> r(len);
  const RingMsgPrefix pre{len, 7};
  std::memcpy(r.data(), &pre, sizeof pre);
  std::memcpy(r.data() + 8, &seq, sizeof seq);
  for (std::uint32_t i = 16; i < len; ++i) r[i] = static_cast<std::byte>((seq + i) & 0xFF);
  return r;
}

std::uint64_t seq_of(const std::byte* p) {
  std::uint64_t s = 0;
  std::memcpy(&s, p + 8, sizeof s);
  return s;
}

std::string ring_path(const char* name) {
  return (tmp_dir() / name).string();
}

}  // namespace

// The protocol is written twice (MsgRing keeps its indices inline for the in-process engine): the
// same randomised pushes and pops, with every wrap and every full ring, must give the same result.
TEST_CASE("core.shm_ring: the same sequence as MsgRing, wraps and full rings included") {
  constexpr std::size_t kCap = 4096;
  MsgRing a(kCap);
  auto created = ShmRing::create(ring_path("same_sequence.ring"), kCap);
  REQUIRE(created.has_value());
  ShmRing& b = *created;
  std::mt19937_64 rng(42);
  std::uint64_t next = 0;
  std::uint64_t pushed = 0;
  std::uint64_t popped = 0;
  for (int step = 0; step < 200'000; ++step) {
    if (rng() % 3 != 0) {
      const auto len = static_cast<std::uint32_t>(64 * (1 + rng() % 6));
      const auto r = record(len, next);
      const bool pa = a.try_push(r.data(), len);
      const bool pb = b.try_push(r.data(), len);
      REQUIRE(pa == pb);
      if (pa) {
        ++next;
        ++pushed;
      }
    } else {
      const std::byte* ma = a.try_peek();
      const std::byte* mb = b.try_peek();
      REQUIRE((ma == nullptr) == (mb == nullptr));
      if (ma != nullptr) {
        const auto len = reinterpret_cast<const RingMsgPrefix*>(ma)->len;
        REQUIRE(reinterpret_cast<const RingMsgPrefix*>(mb)->len == len);
        REQUIRE(std::memcmp(ma, mb, len) == 0);
        REQUIRE(seq_of(ma) == popped);
        a.release();
        b.release();
        ++popped;
      }
    }
  }
  CHECK(pushed > 50'000);
  CHECK(popped > 50'000);
  CHECK(a.bytes_used_approx() == b.bytes_used_approx());
}

// What it is for: a producer in one process, a consumer in another, in order and intact.
TEST_CASE("core.shm_ring: another process reads what this one wrote, in order") {
  const std::string path = ring_path("cross_process.ring");
  auto created = ShmRing::create(path, 1U << 16);
  REQUIRE(created.has_value());
  constexpr std::uint64_t kCount = 200'000;
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    // The consumer opens the file on its own, as an engine attaching to a gateway would.
    auto r = ShmRing::open(path);
    if (!r) ::_exit(10);
    std::uint64_t expect = 0;
    while (expect < kCount) {
      const std::byte* m = r->try_peek();
      if (m == nullptr) continue;
      const auto len = reinterpret_cast<const RingMsgPrefix*>(m)->len;
      const auto want = record(len, expect);
      if (seq_of(m) != expect || std::memcmp(m, want.data(), len) != 0) ::_exit(11);
      r->release();
      ++expect;
    }
    ::_exit(0);
  }
  ShmRing& w = *created;
  for (std::uint64_t i = 0; i < kCount; ++i) {
    const auto len = static_cast<std::uint32_t>(64 * (1 + i % 5));
    const auto r = record(len, i);
    while (!w.try_push(r.data(), len)) {
    }
  }
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("core.shm_ring: a file that is not a ring is refused") {
  const std::string path = ring_path("not_a_ring.ring");
  {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << std::string(8192, 'x');
  }
  CHECK_FALSE(ShmRing::open(path).has_value());
  CHECK_FALSE(ShmRing::open(ring_path("missing.ring")).has_value());
  CHECK_FALSE(ShmRing::create(ring_path("bad_capacity.ring"), 1000).has_value());
  // A ring opened by a second process starts where the first left off.
  auto w = ShmRing::create(ring_path("resume.ring"), 4096);
  REQUIRE(w.has_value());
  const auto r = record(128, 5);
  REQUIRE(w->try_push(r.data(), 128));
  auto reader = ShmRing::open(ring_path("resume.ring"));
  REQUIRE(reader.has_value());
  const std::byte* m = reader->try_peek();
  REQUIRE(m != nullptr);
  CHECK(seq_of(m) == 5);
}

// The engine reads a gateway's rings through the same RingFeed and writes through the same
// LiveTransport as its in-process ones.
TEST_CASE("core.shm_ring: RingFeed and LiveTransport take shared rings next to local ones") {
  MsgRing local(1U << 12);
  auto gw_md = ShmRing::create(ring_path("feed_md.ring"), 1U << 12);
  auto gw_out = ShmRing::create(ring_path("feed_out.ring"), 1U << 12);
  REQUIRE(gw_md.has_value());
  REQUIRE(gw_out.has_value());
  // The engine's own views, as a second process would open them.
  auto md_view = ShmRing::open(ring_path("feed_md.ring"));
  auto out_view = ShmRing::open(ring_path("feed_out.ring"));
  REQUIRE(md_view.has_value());
  REQUIRE(out_view.has_value());

  RingFeed feed;
  REQUIRE(feed.add_ring(&local));
  REQUIRE(feed.add_ring(&*md_view));
  const auto a = record(64, 1);
  const auto b = record(64, 2);
  REQUIRE(local.try_push(a.data(), 64));
  REQUIRE(gw_md->try_push(b.data(), 64));
  CHECK(feed.pending());
  std::vector<std::uint64_t> seen;
  while (const EventHeader* h = feed.next()) {
    seen.push_back(seq_of(reinterpret_cast<const std::byte*>(h)));
    feed.release();
  }
  CHECK(seen == std::vector<std::uint64_t>{1, 2});
  CHECK_FALSE(feed.pending());

  LiveTransport t;
  REQUIRE(t.set_venue(VenueId{0}, &*out_view, false));
  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, InstrumentId{0}, VenueId{0});
  c.cl_ord_id = ClientOrderId{7};
  CHECK(t.send(c.hdr));
  const std::byte* got = gw_out->try_peek();
  REQUIRE(got != nullptr);
  CHECK(reinterpret_cast<const OutCancelMsg*>(got)->cl_ord_id == ClientOrderId{7});
}
