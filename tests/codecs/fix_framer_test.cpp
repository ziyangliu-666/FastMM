#include "fix_test_util.hpp"

using namespace fastmm;
using namespace fastmm::codecs;
using namespace fastmm::codecs::fix;
using namespace fastmm::codecs::fix::test;

namespace {

static_assert(Framer<FixFramer>);

struct Drained {
  std::vector<std::string> messages;
  std::size_t garbage = 0;
  std::size_t left = 0;
};

Drained drain(FixFramer& f, std::string_view stream) {
  Drained d;
  std::size_t off = 0;
  for (;;) {
    const FrameView v = f.next(bytes_of(stream.substr(off)));
    if (!v.complete()) break;
    if (v.kind == FixFramer::kMessage) {
      d.messages.emplace_back(reinterpret_cast<const char*>(v.payload.data()), v.payload.size());
      d.garbage += v.consumed - v.payload.size();
    } else {
      CHECK(v.payload.empty());
      d.garbage += v.consumed;
    }
    off += v.consumed;
  }
  d.left = stream.size() - off;
  return d;
}

const std::string kM1 = wrap("35=0|49=VENUE|56=CLIENT|34=1|52=20260914-08:00:00.000|");
const std::string kM2 =
    wrap("35=1|49=VENUE|56=CLIENT|34=2|52=20260914-08:00:00.000|112=TEST1|58=10=123|");

}  // namespace

TEST_CASE("codecs.fix.framer: frames end at the CheckSum field") {
  FixFramer f;
  const std::string stream = kM1 + kM2;
  const FrameView first = f.next(bytes_of(stream));
  REQUIRE(first.complete());
  CHECK(first.kind == FixFramer::kMessage);
  CHECK(first.consumed == kM1.size());
  CHECK(first.payload.size() == kM1.size());
  const FrameView second = f.next(bytes_of(std::string_view(stream).substr(first.consumed)));
  REQUIRE(second.complete());
  CHECK(second.consumed == kM2.size());
  FixView v;
  CHECK(v.parse(second.payload) == FixError::None);
  CHECK(v.get(tag::kText) == "10=123");  // "10=" inside a value does not end the frame
  CHECK_FALSE(f.next({}).complete());
  CHECK(f.frames() == 2);
}

TEST_CASE("codecs.fix.framer: partial input waits for the whole message") {
  FixFramer f;
  for (std::size_t k = 0; k < kM2.size(); ++k) {
    CAPTURE(k);
    REQUIRE_FALSE(f.next(bytes_of(std::string_view(kM2).substr(0, k))).complete());
  }
  const FrameView v = f.next(bytes_of(kM2));
  REQUIRE(v.complete());
  CHECK(v.consumed == kM2.size());
  CHECK(f.garbage_bytes() == 0);
}

TEST_CASE("codecs.fix.framer: garbage is skipped and the stream resynchronises") {
  SUBCASE("noise before a message") {
    FixFramer f;
    const Drained d = drain(f, "xx\r\nnoise8=FI" + kM1 + "junk" + kM2);
    CHECK(d.messages == std::vector<std::string>{kM1, kM2});
    CHECK(d.garbage == 13 + 4);
    CHECK(d.left == 0);
    CHECK(f.garbage_bytes() == 17);
  }
  SUBCASE("a candidate with a non-numeric BodyLength") {
    FixFramer f;
    const Drained d = drain(f, soh("8=FIX.4.4|9=abc|") + kM1);
    CHECK(d.messages == std::vector<std::string>{kM1});
  }
  SUBCASE("a candidate whose CheckSum is not where BodyLength says") {
    FixFramer f;
    const Drained d = drain(f, soh("8=FIX.4.4|9=5|35=0|xxxxxxxx") + kM1 + kM2);
    CHECK(d.messages == std::vector<std::string>{kM1, kM2});
  }
  SUBCASE("only garbage, then a kept partial prefix") {
    FixFramer f;
    const FrameView g = f.next(bytes_of("hello"));
    REQUIRE(g.complete());
    CHECK(g.kind == FixFramer::kGarbage);
    CHECK(g.consumed == 5);
    const FrameView p = f.next(bytes_of("abc8=F"));
    REQUIRE(p.complete());
    CHECK(p.kind == FixFramer::kGarbage);
    CHECK(p.consumed == 3);
    CHECK_FALSE(f.next(bytes_of("8=F")).complete());
  }
  SUBCASE("a message larger than max_message is garbage") {
    FixFramer f(64);
    const std::string big = wrap(
        "35=0|49=VENUE|56=CLIENT|34=1|52=20260914-08:00:00.000|58=" + std::string(64, 'x') + "|");
    const Drained d = drain(f, big + kM1);
    CHECK(d.messages.size() == 0);  // kM1 itself is 62 bytes < 64 but is still found below
    FixFramer g(128);
    const Drained e = drain(g, big + kM1);
    CHECK(e.messages == std::vector<std::string>{kM1});
  }
}
