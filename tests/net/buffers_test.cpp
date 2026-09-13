#include "test_support.hpp"

#include "fastmm/net/recv_buffer.hpp"
#include "fastmm/net/wire_buffer.hpp"

#include <cstring>
#include <string>

using namespace fastmm::net;

namespace {

void fill(std::span<std::byte> s, char c) {
  std::memset(s.data(), c, s.size());
}

std::string_view sv(std::span<const std::byte> s) {
  return {reinterpret_cast<const char*>(s.data()), s.size()};
}

}  // namespace

TEST_CASE("RecvBuffer: basic commit/consume and padding invariants") {
  RecvBuffer b(1024);
  CHECK(b.capacity() == 1024);
  CHECK(b.empty());
  CHECK(b.writable().size() == 1024);

  auto w = b.writable();
  std::memcpy(w.data(), "hello world", 11);
  b.commit(11);
  CHECK(b.size() == 11);
  CHECK(b.readable_view() == "hello world");
  CHECK(b.free_space() == 1024 - 11);

  b.consume(6);
  CHECK(b.readable_view() == "world");
  CHECK(b.read_offset() == 6);

  // Padding tail is readable and zero (simdjson over-read guarantee).
  const std::byte* pad = b.data() + b.capacity();
  for (std::size_t i = 0; i < RecvBuffer::kPadding; ++i) CHECK(pad[i] == std::byte{0});
  // 64-byte alignment of the storage.
  CHECK(reinterpret_cast<std::uintptr_t>(b.data()) % RecvBuffer::kAlignment == 0);
}

TEST_CASE("RecvBuffer: compaction rules") {
  SUBCASE("reset to zero when fully consumed") {
    RecvBuffer b(256);
    b.commit(100);
    b.consume(100);
    CHECK(b.empty());
    CHECK(b.read_offset() == 100);  // cursors only reset lazily in writable()
    auto w = b.writable();
    CHECK(b.read_offset() == 0);
    CHECK(w.size() == 256);
  }
  SUBCASE("memmove when tail is below the threshold") {
    const std::size_t cap = RecvBuffer::kCompactBelow + 4096;
    RecvBuffer b(cap);
    fill(b.writable().subspan(0, 2048), 'a');
    b.commit(2048);
    b.consume(1024);  // 1024 'a' remain at offset 1024
    // Tail = cap - 2048 >= threshold -> no compaction.
    auto w = b.writable();
    CHECK(b.read_offset() == 1024);
    CHECK(w.size() == cap - 2048);
    // Fill until the tail drops below the threshold.
    fill(w.subspan(0, 4096), 'b');
    b.commit(4096);  // wr = 6144, tail = cap - 6144 = kCompactBelow - 2048 < threshold
    w = b.writable();
    CHECK(b.read_offset() == 0);
    CHECK(b.size() == 1024 + 4096);
    CHECK(w.size() == cap - 5120);
    CHECK(b.readable_view().substr(0, 1024) == std::string(1024, 'a'));
    CHECK(b.readable_view().substr(1024) == std::string(4096, 'b'));
  }
  SUBCASE("explicit compact") {
    RecvBuffer b(256);
    std::memcpy(b.writable().data(), "0123456789", 10);
    b.commit(10);
    b.consume(4);
    b.compact();
    CHECK(b.read_offset() == 0);
    CHECK(b.readable_view() == "456789");
    b.compact();  // no-op when already at the front
    CHECK(b.readable_view() == "456789");
  }
  SUBCASE("clear") {
    RecvBuffer b(64);
    b.commit(10);
    b.clear();
    CHECK(b.empty());
    CHECK(b.writable().size() == 64);
  }
}

TEST_CASE("RecvBuffer: move keeps contents") {
  RecvBuffer a(128);
  std::memcpy(a.writable().data(), "abc", 3);
  a.commit(3);
  RecvBuffer b(std::move(a));
  CHECK(b.readable_view() == "abc");
  CHECK(b.capacity() == 128);
}

TEST_CASE("WireBuffer: append / pending / consumed") {
  WireBuffer w(32);
  CHECK(w.append("hello"));
  CHECK(w.append_u8(0x21));
  CHECK(sv(w.pending()) == "hello!");
  w.consumed(3);
  CHECK(sv(w.pending()) == "lo!");
  w.consumed(3);
  CHECK(w.empty());
  CHECK(w.free_space() == 32);  // cursors reset when drained

  SUBCASE("capacity is enforced, compaction reclaims the head") {
    WireBuffer x(8);
    CHECK(x.append("12345678"));
    CHECK_FALSE(x.append_u8(9));
    x.consumed(4);
    CHECK(x.append("abcd"));  // needed a compaction
    CHECK(sv(x.pending()) == "5678abcd");
    CHECK_FALSE(x.append("z"));
  }
  SUBCASE("write_ptr/commit") {
    WireBuffer x(16);
    REQUIRE(x.reserve(4));
    std::memcpy(x.write_ptr(), "wxyz", 4);
    x.commit(4);
    CHECK(sv(x.pending()) == "wxyz");
  }
}

TEST_CASE("WireBuffer: mask_xor_inplace matches byte-wise reference") {
  const std::uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
  for (std::size_t len : {0u, 1u, 7u, 8u, 9u, 15u, 16u, 100u, 1021u}) {
    CAPTURE(len);
    WireBuffer w(2048);
    w.append("HDR");  // header bytes that must not be touched
    std::string payload(len, '\0');
    for (std::size_t i = 0; i < len; ++i) payload[i] = static_cast<char>(i * 7 + 3);
    w.append(payload);
    w.mask_xor_inplace(3, len, mask);
    auto p = w.pending();
    CHECK(sv(p.subspan(0, 3)) == "HDR");
    for (std::size_t i = 0; i < len; ++i) {
      const auto expect =
          static_cast<std::uint8_t>(static_cast<std::uint8_t>(payload[i]) ^ mask[i % 4]);
      REQUIRE(static_cast<std::uint8_t>(p[3 + i]) == expect);
    }
    // Masking twice restores the original.
    w.mask_xor_inplace(3, len, mask);
    CHECK(sv(w.pending().subspan(3)) == payload);
  }
  SUBCASE("offset is relative to pending() after a partial drain") {
    const std::uint8_t m[4] = {1, 2, 3, 4};
    WireBuffer w(64);
    w.append("xx");
    w.consumed(2);
    w.append(std::string(9, '\0'));
    w.mask_xor_inplace(0, 9, m);
    auto p = w.pending();
    CHECK(static_cast<int>(p[0]) == 1);
    CHECK(static_cast<int>(p[3]) == 4);
    CHECK(static_cast<int>(p[8]) == 1);
  }
}
