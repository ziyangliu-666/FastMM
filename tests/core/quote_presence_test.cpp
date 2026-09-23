#include "fastmm/core/quote_presence.hpp"

#include <doctest/doctest.h>

using namespace fastmm;

namespace {
constexpr InstrumentId kInst{0};
constexpr Timestamp at(std::int64_t ms) noexcept {
  return Timestamp{ms * 1'000'000};
}
}  // namespace

TEST_CASE("core.quote_presence: time counts only while both sides rest") {
  QuotePresence p;
  p.on_live_change(kInst, Side::Buy, +1, at(0));    // bid alone from 0 ms
  p.on_live_change(kInst, Side::Sell, +1, at(100));  // both from 100 ms
  p.on_live_change(kInst, Side::Sell, -1, at(400));  // bid alone again
  p.settle(at(500));

  const QuotePresenceStats s = p.stats(kInst);
  CHECK(s.elapsed_ns == 500'000'000);
  CHECK(s.two_sided_ns == 300'000'000);
  CHECK(s.one_sided_ns == 200'000'000);
  CHECK(s.uptime() == doctest::Approx(0.6));
}

TEST_CASE("core.quote_presence: elapsed starts at the first resting order, not at the session") {
  QuotePresence p;
  p.settle(at(1000));  // nothing has quoted yet
  CHECK(p.stats(kInst).elapsed_ns == 0);
  CHECK(p.stats(kInst).uptime() == 0.0);

  p.on_live_change(kInst, Side::Buy, +1, at(1000));
  p.on_live_change(kInst, Side::Sell, +1, at(1000));
  p.settle(at(1200));
  CHECK(p.stats(kInst).elapsed_ns == 200'000'000);
  CHECK(p.stats(kInst).two_sided_ns == 200'000'000);
}

TEST_CASE("core.quote_presence: a second order on a side keeps the side live until both leave") {
  QuotePresence p;
  p.on_live_change(kInst, Side::Buy, +1, at(0));
  p.on_live_change(kInst, Side::Sell, +1, at(0));
  p.on_live_change(kInst, Side::Sell, +1, at(10));   // two asks rest
  p.on_live_change(kInst, Side::Sell, -1, at(20));   // one leaves; the side is still quoted
  p.settle(at(30));
  CHECK(p.stats(kInst).two_sided_ns == 30'000'000);

  p.on_live_change(kInst, Side::Sell, -1, at(30));  // now the side is empty
  p.settle(at(40));
  CHECK(p.stats(kInst).two_sided_ns == 30'000'000);
  CHECK(p.stats(kInst).one_sided_ns == 10'000'000);
}

TEST_CASE("core.quote_presence: totals add the instruments that quoted") {
  QuotePresence p;
  constexpr InstrumentId other{1};
  p.on_live_change(kInst, Side::Buy, +1, at(0));
  p.on_live_change(kInst, Side::Sell, +1, at(0));
  p.on_live_change(other, Side::Buy, +1, at(0));  // one side only
  p.settle(at(100));

  const QuotePresenceStats t = p.total();
  CHECK(t.elapsed_ns == 200'000'000);
  CHECK(t.two_sided_ns == 100'000'000);
  CHECK(t.one_sided_ns == 100'000'000);
  CHECK(t.uptime() == doctest::Approx(0.5));
}

TEST_CASE("core.quote_presence: a clock that does not move adds nothing") {
  QuotePresence p;
  p.on_live_change(kInst, Side::Buy, +1, at(100));
  p.on_live_change(kInst, Side::Sell, +1, at(100));
  p.settle(at(100));
  p.settle(at(50));  // a backward stamp is ignored rather than subtracted
  CHECK(p.stats(kInst).elapsed_ns == 0);
  p.settle(at(150));
  CHECK(p.stats(kInst).two_sided_ns == 50'000'000);
}
