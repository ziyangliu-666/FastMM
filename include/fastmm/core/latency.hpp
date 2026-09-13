#pragma once
// Latency measurement (5.9): log-linear histogram (no allocation, ~6 % bucket granularity)
// and the per-hop tracker that the engine feeds with rdtscp deltas.
//
// Hops: T0 recv returned, T1 decoded, T2 book applied, T3 strategy decided,
//       T4 order serialised, T5 send returned.
#include "fastmm/core/config_macros.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace fastmm {

// 40 major (power-of-two) buckets x 16 minor buckets. Values < 16 are exact; above that a
// bucket spans 1/16 of its octave (6.25 % relative error). Max value ~2^43 ns (~2.4 h);
// larger values are clamped into the last bucket.
class LogLinearHistogram {
 public:
  static constexpr int kMajor = 40;
  static constexpr int kMinor = 16;
  static constexpr int kBuckets = kMajor * kMinor;

  constexpr LogLinearHistogram() noexcept = default;

  FASTMM_FORCE_INLINE void record(std::uint64_t v) noexcept {
    ++counts_[index_of(v)];
    ++total_;
    sum_ += v;
    if (v > max_) max_ = v;
    if (v < min_) min_ = v;
  }
  void reset() noexcept { *this = LogLinearHistogram{}; }
  void merge(const LogLinearHistogram& o) noexcept {
    for (int i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
    total_ += o.total_;
    sum_ += o.sum_;
    if (o.max_ > max_) max_ = o.max_;
    if (o.min_ < min_) min_ = o.min_;
  }

  [[nodiscard]] std::uint64_t count() const noexcept { return total_; }
  [[nodiscard]] std::uint64_t max() const noexcept { return total_ == 0 ? 0 : max_; }
  [[nodiscard]] std::uint64_t min() const noexcept { return total_ == 0 ? 0 : min_; }
  [[nodiscard]] std::uint64_t mean() const noexcept { return total_ == 0 ? 0 : sum_ / total_; }

  // Value at quantile q in [0, 1]: upper bound of the bucket containing the q-th sample.
  // (q = 0.5 -> p50). Exact for values < 16; within +6.25 % above.
  [[nodiscard]] std::uint64_t percentile(double q) const noexcept {
    if (total_ == 0) return 0;
    if (q <= 0.0) return min_;
    if (q >= 1.0) return max_;
    const auto target = static_cast<std::uint64_t>(static_cast<double>(total_) * q);
    std::uint64_t acc = 0;
    for (int i = 0; i < kBuckets; ++i) {
      acc += counts_[i];
      if (acc > target) {
        const std::uint64_t hi = upper_bound(i);
        return hi > max_ ? max_ : hi;
      }
    }
    return max_;
  }

  [[nodiscard]] std::uint64_t bucket_count(int i) const noexcept { return counts_[i]; }
  [[nodiscard]] static constexpr std::uint64_t lower_bound(int i) noexcept {
    const int major = i / kMinor;
    const int minor = i % kMinor;
    if (major == 0) return static_cast<std::uint64_t>(minor);
    return (std::uint64_t{16} + static_cast<std::uint64_t>(minor)) << (major - 1);
  }
  [[nodiscard]] static constexpr std::uint64_t upper_bound(int i) noexcept {  // inclusive
    return i + 1 < kBuckets ? lower_bound(i + 1) - 1 : UINT64_MAX;
  }
  [[nodiscard]] static constexpr int index_of(std::uint64_t v) noexcept {
    if (v < 16) return static_cast<int>(v);
    const int msb = 63 - std::countl_zero(v);  // >= 4
    int major = msb - 3;                       // 1.. ; msb 4 -> major 1
    if (major >= kMajor) return kBuckets - 1;
    const int minor = static_cast<int>((v >> (msb - 4)) & 15U);
    return major * kMinor + minor;
  }

 private:
  std::uint64_t counts_[kBuckets] = {};
  std::uint64_t total_ = 0;
  std::uint64_t sum_ = 0;
  std::uint64_t max_ = 0;
  std::uint64_t min_ = UINT64_MAX;
};

enum class LatencyHop : std::uint8_t {
  T0Recv = 0,
  T1Decoded,
  T2BookApplied,
  T3Decision,
  T4Serialized,
  T5Sent,
  Count
};

// The intervals we histogram; each is (from hop, to hop).
enum class LatencyInterval : std::uint8_t {
  Decode = 0,       // T0 -> T1
  BookApply = 1,    // T1 -> T2
  Strategy = 2,     // T2 -> T3
  Serialize = 3,    // T3 -> T4
  Send = 4,         // T4 -> T5
  TickToTrade = 5,  // T0 -> T5
  WireToBook = 6,   // T0 -> T2
  Count = 7,
};
[[nodiscard]] constexpr const char* to_string(LatencyInterval i) noexcept {
  switch (i) {
    case LatencyInterval::Decode:
      return "decode";
    case LatencyInterval::BookApply:
      return "book_apply";
    case LatencyInterval::Strategy:
      return "strategy";
    case LatencyInterval::Serialize:
      return "serialize";
    case LatencyInterval::Send:
      return "send";
    case LatencyInterval::TickToTrade:
      return "tick_to_trade";
    case LatencyInterval::WireToBook:
      return "wire_to_book";
    case LatencyInterval::Count:
      return "count";
  }
  return "?";
}

struct LatencyStats {
  std::uint64_t count = 0;
  std::uint64_t p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0, mean = 0;
};

struct LatencySnapshot {
  std::int64_t ts_ns = 0;
  LatencyStats interval[static_cast<std::size_t>(LatencyInterval::Count)];
};
static_assert(std::is_trivially_copyable_v<LatencySnapshot>);

// Owned by the engine thread. record() takes nanoseconds (the engine converts cycles).
class LatencyTracker {
 public:
  FASTMM_FORCE_INLINE void record(LatencyInterval i, std::uint64_t ns) noexcept {
    hist_[static_cast<std::size_t>(i)].record(ns);
  }
  [[nodiscard]] const LogLinearHistogram& histogram(LatencyInterval i) const noexcept {
    return hist_[static_cast<std::size_t>(i)];
  }
  [[nodiscard]] LatencySnapshot snapshot(std::int64_t ts_ns) const noexcept {
    LatencySnapshot s;
    s.ts_ns = ts_ns;
    for (std::size_t i = 0; i < static_cast<std::size_t>(LatencyInterval::Count); ++i) {
      const auto& h = hist_[i];
      auto& st = s.interval[i];
      st.count = h.count();
      st.p50 = h.percentile(0.50);
      st.p90 = h.percentile(0.90);
      st.p99 = h.percentile(0.99);
      st.p999 = h.percentile(0.999);
      st.max = h.max();
      st.mean = h.mean();
    }
    return s;
  }
  void reset() noexcept {
    for (auto& h : hist_) h.reset();
  }

 private:
  LogLinearHistogram hist_[static_cast<std::size_t>(LatencyInterval::Count)];
};

// Exporters (src/core/latency_export.cpp; allocate, not for the hot path).
[[nodiscard]] std::string format_latency_text(const LatencySnapshot& s);
[[nodiscard]] std::string format_latency_csv(const LatencySnapshot& s, bool header = true);
[[nodiscard]] std::string format_latency_prometheus(const LatencySnapshot& s,
                                                    const char* prefix = "fastmm");

}  // namespace fastmm
