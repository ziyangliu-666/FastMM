#pragma once
// LatencyModel (8.2): per-direction delay = fixed + jitter * LogNormal(mean 1), plus an
// independent drop probability for the order path. All draws come from one seeded
// Xoshiro256ss so a run is reproducible bit for bit; the double math is confined to the
// simulator (delays are converted to integer nanoseconds immediately).
#include "fastmm/core/rng.hpp"
#include "fastmm/core/time.hpp"

#include <cmath>
#include <cstdint>

namespace fastmm::sim {

struct LatencyParams {
  Duration fixed{};     // deterministic floor
  Duration jitter{};    // mean of the lognormal excess (0 = none)
  double p_drop = 0.0;  // probability the message is lost (order path only)
  double sigma = 0.5;   // lognormal shape; E[excess] == jitter for any sigma
};

struct LatencySample {
  Duration delay{};
  bool dropped = false;
};

class LatencyModel {
 public:
  LatencyModel() noexcept = default;
  LatencyModel(const LatencyParams& order_out,
               const LatencyParams& ack_in,
               const LatencyParams& md_in,
               std::uint64_t seed) noexcept
      : order_out_(order_out), ack_in_(ack_in), md_in_(md_in), rng_(seed) {}

  // Engine -> venue (orders, cancels, replaces).
  [[nodiscard]] LatencySample order_out() noexcept {
    LatencySample s;
    s.delay = draw(order_out_);
    s.dropped = order_out_.p_drop > 0.0 && rng_.uniform01() < order_out_.p_drop;
    return s;
  }
  // Venue -> engine (acks, fills).
  [[nodiscard]] Duration ack_in() noexcept { return draw(ack_in_); }
  // Venue -> engine (market data).
  [[nodiscard]] Duration md_in() noexcept { return draw(md_in_); }

  [[nodiscard]] const LatencyParams& order_out_params() const noexcept { return order_out_; }
  [[nodiscard]] const LatencyParams& ack_in_params() const noexcept { return ack_in_; }
  [[nodiscard]] const LatencyParams& md_in_params() const noexcept { return md_in_; }
  [[nodiscard]] Xoshiro256ss& rng() noexcept { return rng_; }

 private:
  [[nodiscard]] Duration draw(const LatencyParams& p) noexcept {
    if (p.jitter.ns <= 0) return p.fixed;
    // Box-Muller; u1 in (0, 1] so the log is finite.
    const double u1 = 1.0 - rng_.uniform01();
    const double u2 = rng_.uniform01();
    const double z = std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    const double s = p.sigma;
    const double excess = static_cast<double>(p.jitter.ns) * std::exp(s * z - 0.5 * s * s);
    return p.fixed + Duration{static_cast<std::int64_t>(excess)};
  }

  LatencyParams order_out_{};
  LatencyParams ack_in_{};
  LatencyParams md_in_{};
  Xoshiro256ss rng_{1};
};

}  // namespace fastmm::sim
