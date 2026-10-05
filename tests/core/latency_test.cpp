#include "fastmm/core/latency.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

using namespace fastmm;

TEST_CASE("core.latency: bucket bounds are contiguous and index_of is consistent") {
  for (int i = 0; i + 1 < LogLinearHistogram::kBuckets; ++i) {
    CHECK(LogLinearHistogram::upper_bound(i) + 1 == LogLinearHistogram::lower_bound(i + 1));
    CHECK(LogLinearHistogram::index_of(LogLinearHistogram::lower_bound(i)) == i);
    CHECK(LogLinearHistogram::index_of(LogLinearHistogram::upper_bound(i)) == i);
  }
  CHECK(LogLinearHistogram::index_of(0) == 0);
  CHECK(LogLinearHistogram::index_of(15) == 15);
  CHECK(LogLinearHistogram::index_of(16) == 16);
  CHECK(LogLinearHistogram::index_of(UINT64_MAX) == LogLinearHistogram::kBuckets - 1);
}

TEST_CASE("core.latency: percentiles vs sorted vector within bucket granularity") {
  LogLinearHistogram h;
  CHECK(h.percentile(0.5) == 0);
  std::mt19937_64 rng(7);
  std::lognormal_distribution<double> dist(7.0, 1.2);  // ~1 µs-ish with a long tail
  std::vector<std::uint64_t> v;
  for (int i = 0; i < 200'000; ++i) {
    const auto x = static_cast<std::uint64_t>(dist(rng));
    v.push_back(x);
    h.record(x);
  }
  std::sort(v.begin(), v.end());
  CHECK(h.count() == v.size());
  CHECK(h.max() == v.back());
  CHECK(h.min() == v.front());
  for (double q : {0.5, 0.9, 0.99, 0.999}) {
    const auto exact = v[static_cast<std::size_t>(static_cast<double>(v.size()) * q)];
    const auto est = h.percentile(q);
    CAPTURE(q);
    CAPTURE(exact);
    CAPTURE(est);
    // upper bound of the bucket: never below the exact value, at most 6.25 % + 1 above
    CHECK(est >= exact);
    CHECK(static_cast<double>(est) <= static_cast<double>(exact) * 1.0625 + 1.0);
  }
  CHECK(h.percentile(1.0) == v.back());
  CHECK(h.percentile(0.0) == v.front());
  const auto mean = h.mean();
  std::uint64_t sum = 0;
  for (auto x : v) sum += x;
  CHECK(mean == sum / v.size());

  LogLinearHistogram h2;
  h2.record(5);
  h2.merge(h);
  CHECK(h2.count() == v.size() + 1);
  h2.reset();
  CHECK(h2.count() == 0);
}

TEST_CASE("core.latency: tracker snapshot and exporters") {
  LatencyTracker t;
  for (std::uint64_t i = 1; i <= 1000; ++i) t.record(LatencyInterval::TickToTrade, i * 10);
  t.record(LatencyInterval::Decode, 7);
  const LatencySnapshot s = t.snapshot(123);
  CHECK(s.ts_ns == 123);
  const auto& tt = s.interval[static_cast<int>(LatencyInterval::TickToTrade)];
  CHECK(tt.count == 1000);
  CHECK(tt.max == 10000);
  CHECK(tt.p50 >= 5000);
  CHECK(tt.p50 <= 5400);
  CHECK(s.interval[static_cast<int>(LatencyInterval::Decode)].p50 == 7);
  const std::string text = format_latency_text(s);
  CHECK(text.find("tick_to_trade") != std::string::npos);
  const std::string csv = format_latency_csv(s);
  CHECK(csv.find("ts_ns,interval,count") == 0);
  CHECK(csv.find("123,tick_to_trade,1000,") != std::string::npos);
  const std::string prom = format_latency_prometheus(s);
  CHECK(prom.find("fastmm_latency_ns{interval=\"decode\",quantile=\"0.5\"} 7") !=
        std::string::npos);
  t.reset();
  CHECK(t.histogram(LatencyInterval::Decode).count() == 0);
}

TEST_CASE(
    "core.latency: RecentHistogram keeps the last minute and the last hour beside the total") {
  constexpr std::int64_t kS = 1'000'000'000;
  auto h = std::make_unique<RecentHistogram>();
  h->roll(0);
  // A slow first minute, then fast samples only.
  for (int i = 0; i < 100; ++i) h->record(1'000'000);
  for (std::int64_t t = 1; t <= 70; ++t) {
    h->roll(t * kS);
    h->record(1'000);
  }
  // The slow ones closed with the first slice, 70 s ago: out of the minute, in the hour.
  CHECK(h->total().count() == 170);
  CHECK(h->last_hour().count() == 170);
  CHECK(h->last_hour().percentile(0.99) >= 1'000'000);
  const LogLinearHistogram m = h->last_minute();
  CHECK(m.count() >= 60);
  CHECK(m.count() <= 70);
  CHECK(m.max() < 2'000);
  CHECK(m.percentile(0.99) < 2'000);
  // Seventy minutes on, the hour has let them go too; the total keeps them.
  for (std::int64_t t = 71; t <= 71 * 60; t += 5) h->roll(t * kS);
  CHECK(h->last_hour().count() == 0);
  CHECK(h->last_minute().count() == 0);
  CHECK(h->total().count() == 170);
  // A sample, then a day without a roll: it went into the slice the day starts with, gone from
  // both windows, kept in the total.
  h->record(5);
  CHECK(h->last_minute().count() == 1);
  h->roll(86'400 * kS);
  CHECK(h->last_minute().count() == 0);
  CHECK(h->last_hour().count() == 0);
  CHECK(h->total().count() == 171);
  h->reset();
  CHECK(h->total().count() == 0);
}
