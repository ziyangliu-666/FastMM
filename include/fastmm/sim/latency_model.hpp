#pragma once
// LatencyModel (8.2): per-direction delay = fixed + jitter * LogNormal(mean 1), plus an
// independent drop probability for the order path. Cancels take the order path, or a path of their
// own when one is given (a venue may take a cancel in faster than a new order); fills and balances
// likewise reach the engine on the order path's replies or on a path of their own (a venue's
// private stream, whose tail is heavier than its order API's replies). The draws come from seeded
// Xoshiro256ss streams so a run is reproducible bit for bit; the double math is confined to the
// simulator (delays are converted to integer nanoseconds immediately). Market data draws from a
// stream of its own (MdPath): the feed's delays depend on the seed and the data alone, not on the
// orders a strategy sends, so the runs of a sweep with one seed see one feed and share it
// (backtest/multiverse.hpp).
#include "fastmm/core/config_macros.hpp"
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

// jitter * LogNormal(mean 1), drawn from `rng`.
[[nodiscard]] FASTMM_NOINLINE inline Duration draw_jitter(const LatencyParams& p,
                                                          Xoshiro256ss& rng) noexcept {
  // Box-Muller; u1 in (0, 1] so the log is finite.
  const double u1 = 1.0 - rng.uniform01();
  const double u2 = rng.uniform01();
  const double z = std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  const double s = p.sigma;
  const double excess = static_cast<double>(p.jitter.ns) * std::exp(s * z - 0.5 * s * s);
  return Duration{static_cast<std::int64_t>(excess)};
}

// fixed + jitter * LogNormal(mean 1), drawn from `rng` (none when there is no jitter).
[[nodiscard]] FASTMM_FORCE_INLINE Duration draw_latency(const LatencyParams& p,
                                                        Xoshiro256ss& rng) noexcept {
  if (p.jitter.ns <= 0) return p.fixed;
  return p.fixed + draw_jitter(p, rng);
}

// The market-data path of a venue: its latency and the stream its jitter draws from. Two equal
// paths give the same delays for the same messages.
struct MdPath {
  LatencyParams params{};
  Xoshiro256ss rng{};
  [[nodiscard]] Duration draw() noexcept { return draw_latency(params, rng); }
  friend bool operator==(const MdPath& a, const MdPath& b) noexcept {
    return a.params.fixed == b.params.fixed && a.params.jitter == b.params.jitter &&
           a.params.sigma == b.params.sigma && (a.params.jitter.ns <= 0 || a.rng == b.rng);
  }
};

class LatencyModel {
 public:
  LatencyModel() noexcept = default;
  LatencyModel(const LatencyParams& order_out,
               const LatencyParams& ack_in,
               const LatencyParams& md_in,
               std::uint64_t seed) noexcept
      : order_out_(order_out),
        ack_in_(ack_in),
        md_{md_in, Xoshiro256ss(md_seed(seed))},
        rng_(seed) {}
  // `cancel_out`: the cancels' own engine -> venue path (its p_drop is the order path's).
  LatencyModel(const LatencyParams& order_out,
               const LatencyParams& ack_in,
               const LatencyParams& md_in,
               const LatencyParams& cancel_out,
               std::uint64_t seed) noexcept
      : order_out_(order_out),
        ack_in_(ack_in),
        md_{md_in, Xoshiro256ss(md_seed(seed))},
        cancel_out_(cancel_out),
        own_cancel_(true),
        rng_(seed) {}

  // Engine -> venue (orders, cancels, replaces).
  [[nodiscard]] LatencySample order_out() noexcept {
    LatencySample s;
    s.delay = draw(order_out_);
    s.dropped = order_out_.p_drop > 0.0 && rng_.uniform01() < order_out_.p_drop;
    return s;
  }
  // Engine -> venue, a cancel.
  [[nodiscard]] LatencySample cancel_out() noexcept {
    if (!own_cancel_) return order_out();
    LatencySample s;
    s.delay = draw(cancel_out_);
    s.dropped = order_out_.p_drop > 0.0 && rng_.uniform01() < order_out_.p_drop;
    return s;
  }
  // Venue -> engine (acks, fills).
  [[nodiscard]] Duration ack_in() noexcept { return draw(ack_in_); }
  // Venue -> engine, a fill or a balance on the private stream (set_fill_in), else ack_in().
  [[nodiscard]] Duration fill_in() noexcept { return own_fill_ ? draw(fill_in_) : ack_in(); }
  void set_fill_in(const LatencyParams& p) noexcept {
    fill_in_ = p;
    own_fill_ = true;
  }
  [[nodiscard]] bool own_fill() const noexcept { return own_fill_; }
  [[nodiscard]] const LatencyParams& fill_in_params() const noexcept {
    return own_fill_ ? fill_in_ : ack_in_;
  }
  // Venue -> engine (market data), from the market data's own stream.
  [[nodiscard]] Duration md_in() noexcept { return md_.draw(); }
  [[nodiscard]] const MdPath& md_path() const noexcept { return md_; }

  [[nodiscard]] const LatencyParams& order_out_params() const noexcept { return order_out_; }
  [[nodiscard]] const LatencyParams& ack_in_params() const noexcept { return ack_in_; }
  [[nodiscard]] const LatencyParams& md_in_params() const noexcept { return md_.params; }
  [[nodiscard]] Xoshiro256ss& rng() noexcept { return rng_; }

 private:
  [[nodiscard]] Duration draw(const LatencyParams& p) noexcept { return draw_latency(p, rng_); }
  // The market data's stream: another seed from the same one.
  [[nodiscard]] static constexpr std::uint64_t md_seed(std::uint64_t seed) noexcept {
    return seed ^ 0x6D61726B65746464ULL;
  }

  LatencyParams order_out_{};
  LatencyParams ack_in_{};
  MdPath md_{};
  LatencyParams cancel_out_{};
  LatencyParams fill_in_{};
  bool own_cancel_ = false;
  bool own_fill_ = false;
  Xoshiro256ss rng_{1};
};

}  // namespace fastmm::sim
