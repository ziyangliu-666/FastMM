#include "test_support.hpp"

#include "fastmm/codecs/codec.hpp"

#include <array>
#include <cstring>

using namespace fastmm;
using namespace fastmm::codecs;

namespace {
#pragma pack(push, 1)
struct PackedHeader {
  be16_t length;
  char type;
  be32_t seq;
  le64_t stamp;
};
#pragma pack(pop)
static_assert(sizeof(PackedHeader) == 15);

struct LengthPrefixFramer {  // 2-byte big-endian length prefix
  FrameView next(std::span<const std::byte> in) noexcept {
    if (in.size() < 2) return {};
    const std::size_t n =
        (std::to_integer<std::size_t>(in[0]) << 8) | std::to_integer<std::size_t>(in[1]);
    if (in.size() < 2 + n) return {};
    return FrameView{in.subspan(2, n), 2 + n, 0};
  }
};
static_assert(Framer<LengthPrefixFramer>);
}  // namespace

TEST_CASE("codecs: endian helpers and packed layouts") {
  PackedHeader h{};
  h.length.set(0x0102);
  h.type = 'A';
  h.seq.set(0x0A0B0C0D);
  h.stamp.set(0x1122334455667788ULL);
  std::array<unsigned char, sizeof(PackedHeader)> bytes{};
  std::memcpy(bytes.data(), &h, sizeof h);
  CHECK(bytes[0] == 0x01);
  CHECK(bytes[1] == 0x02);
  CHECK(bytes[2] == 'A');
  CHECK(bytes[3] == 0x0A);
  CHECK(bytes[6] == 0x0D);
  CHECK(bytes[7] == 0x88);  // little-endian
  CHECK(h.seq.get() == 0x0A0B0C0D);
  CHECK(h.stamp.get() == 0x1122334455667788ULL);
  CHECK(library_name() == "fastmm::codecs");
}

TEST_CASE("codecs: a framer waits for complete frames") {
  LengthPrefixFramer f;
  const std::array<std::byte, 5> partial{
      std::byte{0}, std::byte{3}, std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  CHECK_FALSE(f.next(std::span<const std::byte>(partial).first(4)).complete());
  const FrameView v = f.next(partial);
  REQUIRE(v.complete());
  CHECK(v.consumed == 5);
  CHECK(v.payload.size() == 3);
}
