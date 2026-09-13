// The venues' network-thread order latency recording (venues::WireLatencyRecorder) runs once
// per outbound order on the reactor thread: it must not allocate, and neither may the summary
// the venue builds when it publishes its status.
#include "alloc_counter.hpp"
#include "test_support.hpp"

#include "fastmm/core/time.hpp"
#include "fastmm/venues/wire_latency.hpp"

#include <memory>

using namespace fastmm;
using fastmm::test::NoAllocScope;

TEST_CASE("hotpath.noalloc: venue wire latency recording and summary") {
  auto rec = std::make_unique<venues::WireLatencyRecorder>();
  const TscCalibration cal = calibrate_tsc(milliseconds(5));
  venues::WireLatencyStats t2t;
  venues::WireLatencyStats encode;
  venues::WireLatencyStats send;
  {
    NoAllocScope guard(true);
    for (int i = 0; i < 10'000; ++i) {
      const Cycles t0 = (i & 1) != 0 ? rdtscp() : Cycles{};
      const Cycles before_encode = rdtscp();
      const Cycles after_encode = rdtscp();
      rec->record(t0, before_encode, after_encode, rdtscp());
    }
    rec->summarize(cal, t2t, encode, send);
  }
  CHECK(encode.count == 10'000);
  CHECK(send.count == 10'000);
  CHECK(t2t.count == 5'000);
}
