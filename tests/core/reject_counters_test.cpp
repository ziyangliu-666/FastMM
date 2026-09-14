#include "fastmm/core/reject_counters.hpp"

#include "test_support.hpp"

#include <cstdint>

using namespace fastmm;

TEST_CASE("core.reject_counters: counts per reason, totals and the formatted breakdown") {
  RejectCounts c;
  CHECK(c.total() == 0);
  CHECK(format_reject_counts(c).empty());
  CHECK(nonzero_rejects(c).empty());
  c.add(RejectReason::RateLimit);
  c.add(RejectReason::MaxPosition);
  c.add(RejectReason::MaxPosition);
  c.add(RejectReason::PriceCollar);
  c.add(RejectReason::VenueUnknownOrder);  // the last slot
  CHECK(c[RejectReason::MaxPosition] == 2);
  CHECK(c[RejectReason::RateLimit] == 1);
  CHECK(c[RejectReason::MaxOrderQty] == 0);
  CHECK(c[RejectReason::VenueUnknownOrder] == 1);
  CHECK(c.total() == 5);
  // Most frequent first, equal counts in enum order.
  CHECK(format_reject_counts(c) ==
        "MaxPosition 2, PriceCollar 1, RateLimit 1, VenueUnknownOrder 1");
  // A value outside the enum is counted as a generic venue reject, never out of bounds.
  c.add(static_cast<RejectReason>(200));
  CHECK(c[RejectReason::VenueReject] == 1);
  CHECK(c.total() == 6);
}

TEST_CASE("core.reject_counters: log limiter admits the first reject, then one per interval") {
  RejectLogLimiter lim(seconds(10));
  const Timestamp t0{seconds(1000).ns};
  std::uint64_t suppressed = 99;
  CHECK(lim.admit(RejectReason::MaxPosition, t0, suppressed));
  CHECK(suppressed == 0);
  CHECK_FALSE(lim.admit(RejectReason::MaxPosition, t0 + seconds(1), suppressed));
  CHECK_FALSE(lim.admit(RejectReason::MaxPosition, t0 + seconds(9), suppressed));
  // Another reason has its own budget.
  CHECK(lim.admit(RejectReason::RateLimit, t0 + seconds(2), suppressed));
  CHECK(suppressed == 0);
  CHECK(lim.admit(RejectReason::MaxPosition, t0 + seconds(10), suppressed));
  CHECK(suppressed == 2);
  CHECK_FALSE(lim.admit(RejectReason::MaxPosition, t0 + seconds(11), suppressed));
  // A clock that went backwards ends the quiet period.
  CHECK(lim.admit(RejectReason::MaxPosition, t0, suppressed));
  CHECK(suppressed == 1);

  RejectLogLimiter every(Duration{});
  CHECK(every.admit(RejectReason::PriceCollar, t0, suppressed));
  CHECK(every.admit(RejectReason::PriceCollar, t0, suppressed));
  CHECK(suppressed == 0);
}
