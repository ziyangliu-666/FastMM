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

TEST_CASE("wire latency: a batch records every order with the stamp after the batch's write") {
  auto rec = std::make_unique<venues::WireLatencyRecorder>();
  {
    NoAllocScope guard(true);
    rec->begin_batch();
    for (std::uint64_t i = 0; i < 3; ++i) {
      rec->record(Cycles{100 + i}, Cycles{1000 + (i * 100)}, Cycles{1050 + (i * 100)}, Cycles{1});
    }
    CHECK(rec->send().count() == 0);  // kept until end_batch
    rec->end_batch(Cycles{5000}, true);
  }
  CHECK(rec->encode().count() == 3);
  CHECK(rec->send().count() == 3);
  CHECK(rec->tick_to_trade().count() == 3);
  CHECK(rec->send().max() >= 5000 - 1250);
  CHECK(rec->tick_to_trade().max() >= 5000 - 100);

  // A failed write drops the batch; outside a batch record() records at once.
  rec->reset();
  rec->begin_batch();
  rec->record(Cycles{1}, Cycles{10}, Cycles{20}, Cycles{30});
  rec->end_batch(Cycles{40}, false);
  CHECK(rec->send().count() == 0);
  rec->record(Cycles{1}, Cycles{10}, Cycles{20}, Cycles{30});
  CHECK(rec->send().count() == 1);

  // More than kMaxBatch orders in one batch: the excess is recorded with its own stamp.
  rec->reset();
  rec->begin_batch();
  for (std::size_t i = 0; i < venues::WireLatencyRecorder::kMaxBatch + 2; ++i)
    rec->record(Cycles{}, Cycles{10}, Cycles{20}, Cycles{30});
  CHECK(rec->batch_full());
  CHECK(rec->send().count() == 2);
  rec->end_batch(Cycles{40}, true);
  CHECK(rec->send().count() == venues::WireLatencyRecorder::kMaxBatch + 2);
}
