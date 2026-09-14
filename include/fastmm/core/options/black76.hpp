#pragma once
// Black-76 option pricing, greeks and implied volatility (options on a forward / futures price).
//
//   d1 = (ln(F / K) + sigma^2 T / 2) / (sigma sqrt(T)),   d2 = d1 - sigma sqrt(T),   D = e^{-rT}
//   call = D (F N(d1) - K N(d2)),                          put  = D (K N(-d2) - F N(-d1))
//
// Conventions (per unit of the underlying; multiply by position size and contract multiplier):
//   delta = dV/dF              call D N(d1), put -D N(-d1)
//   gamma = d2V/dF2            D n(d1) / (F sigma sqrt(T))
//   vega  = dV/dsigma          D F n(d1) sqrt(T)          (per 1.00 of vol: /100 for a vol point)
//   theta = dV/dt              -D F n(d1) sigma / (2 sqrt(T)) + r V   (per year of calendar time)
//   rho   = dV/dr              -T V                        (Black-76: the forward does not move)
// Reference: F. Black, "The pricing of commodity contracts", J. Financial Economics 3 (1976);
// E. G. Haug, "The Complete Guide to Option Pricing Formulas" (2nd ed.), Black-76 example
// F = K = 19, T = 0.75, r = 0.10, sigma = 0.28 -> call = put = 1.7011
// (tests/core/black76_test.cpp).
//
// Deribit (ticker channel, verified against a recorded testnet ticker) reports the same model
// with r = interest_rate (0): delta = D N(d1), gamma per USD, vega / 100 (per vol point), theta /
// 365 (per day) in USD, and rho as the spot-model K T N(d2) / 100 rather than Black-76 rho.
// Inverse (coin-margined) options are quoted in the base coin: price_coin = V / F, and their
// delta in coin terms is delta - price_coin (the premium itself moves with F).
//
// Everything is noexcept, allocation-free and uses only <cmath>; the implied-volatility solver is
// a bracketed Newton iteration with bisection fallback and a hard iteration bound.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace fastmm::options {

inline constexpr double kSecondsPerYear = 365.0 * 86400.0;  // ACT/365, as Deribit's theta
inline constexpr double kMinVol = 1e-4;                     // 0.01 % annualised
inline constexpr double kMaxVol = 10.0;                     // 1000 % annualised

enum class CallPut : std::uint8_t { Call = 0, Put = 1 };

// Standard normal density and distribution. erfc keeps the far tail accurate (no 1 - N(x)).
[[nodiscard]] inline double norm_pdf(double x) noexcept {
  return 0.398942280401432677939946 * std::exp(-0.5 * x * x);  // 1 / sqrt(2 pi)
}
[[nodiscard]] inline double norm_cdf(double x) noexcept {
  return 0.5 * std::erfc(-x * 0.707106781186547524400844);  // 1 / sqrt(2)
}

// Year fraction between two epoch-ns timestamps (ACT/365); negative once expired.
[[nodiscard]] constexpr double year_fraction(std::int64_t expiry_ns, std::int64_t now_ns) noexcept {
  return static_cast<double>(expiry_ns - now_ns) / 1e9 / kSecondsPerYear;
}

struct Greeks {
  double price = 0.0;
  double delta = 0.0;
  double gamma = 0.0;
  double vega = 0.0;
  double theta = 0.0;
  double rho = 0.0;
};

// Price and greeks. Degenerate inputs (T <= 0 or sigma <= 0) return the discounted intrinsic
// value with a step delta and zero gamma/vega/theta; F <= 0 or K <= 0 return all zeros.
[[nodiscard]] inline Greeks black76(CallPut cp,
                                    double forward,
                                    double strike,
                                    double t_years,
                                    double sigma,
                                    double rate = 0.0) noexcept {
  Greeks g;
  if (!(forward > 0.0) || !(strike > 0.0)) return g;
  const double df = std::exp(-rate * (t_years > 0.0 ? t_years : 0.0));
  const bool call = cp == CallPut::Call;
  if (!(t_years > 0.0) || !(sigma > 0.0)) {
    const double intrinsic = call ? forward - strike : strike - forward;
    if (intrinsic > 0.0) {
      g.price = df * intrinsic;
      g.delta = call ? df : -df;
    }
    return g;
  }
  const double sqrt_t = std::sqrt(t_years);
  const double sd = sigma * sqrt_t;
  const double d1 = (std::log(forward / strike) + 0.5 * sd * sd) / sd;
  const double d2 = d1 - sd;
  const double pdf = norm_pdf(d1);
  if (call) {
    g.price = df * (forward * norm_cdf(d1) - strike * norm_cdf(d2));
    g.delta = df * norm_cdf(d1);
  } else {
    g.price = df * (strike * norm_cdf(-d2) - forward * norm_cdf(-d1));
    g.delta = -df * norm_cdf(-d1);
  }
  g.gamma = df * pdf / (forward * sd);
  g.vega = df * forward * pdf * sqrt_t;
  g.theta = -df * forward * pdf * sigma / (2.0 * sqrt_t) + rate * g.price;
  g.rho = -t_years * g.price;
  return g;
}

[[nodiscard]] inline double black76_price(CallPut cp,
                                          double forward,
                                          double strike,
                                          double t_years,
                                          double sigma,
                                          double rate = 0.0) noexcept {
  return black76(cp, forward, strike, t_years, sigma, rate).price;
}

enum class IvStatus : std::uint8_t {
  Ok = 0,
  AtIntrinsic = 1,    // price at (or within tolerance of) the no-arbitrage lower bound: vol 0
  OutOfBounds = 2,    // below intrinsic, or above what kMaxVol can produce
  BadInput = 3,       // non-positive forward/strike/time or non-finite price
  NoConvergence = 4,  // iteration bound hit; vol is the best bracket midpoint
};

struct IvResult {
  double vol = 0.0;
  IvStatus status = IvStatus::BadInput;
  int iterations = 0;
  [[nodiscard]] bool ok() const noexcept { return status == IvStatus::Ok; }
};

// Implied volatility of a Black-76 price. `price_tol` is the absolute price tolerance (same unit
// as `price`); iterations never exceed `max_iterations`.
[[nodiscard]] inline IvResult implied_vol(CallPut cp,
                                          double price,
                                          double forward,
                                          double strike,
                                          double t_years,
                                          double rate = 0.0,
                                          double price_tol = 1e-12,
                                          int max_iterations = 100) noexcept {
  IvResult r;
  if (!(forward > 0.0) || !(strike > 0.0) || !(t_years > 0.0) || !std::isfinite(price) ||
      !std::isfinite(rate))
    return r;
  const bool call = cp == CallPut::Call;
  // Work undiscounted: the bounds are then [max(F-K, 0), F] for calls, [max(K-F, 0), K] for puts.
  const double p = price * std::exp(rate * t_years);
  const double intrinsic = call ? std::max(forward - strike, 0.0) : std::max(strike - forward, 0.0);
  const double upper = call ? forward : strike;
  const double tol = price_tol > 0.0 ? price_tol : 1e-12;
  if (p < intrinsic - tol || p >= upper) {
    r.status = IvStatus::OutOfBounds;
    return r;
  }
  double lo = kMinVol;
  double hi = kMaxVol;
  if (p <= black76_price(cp, forward, strike, t_years, lo) + tol) {
    r.status = IvStatus::AtIntrinsic;
    return r;
  }
  if (p > black76_price(cp, forward, strike, t_years, hi)) {
    r.status = IvStatus::OutOfBounds;
    return r;
  }
  // Brenner-Subrahmanyam start (exact at the money), kept inside the bracket.
  double sigma =
      std::sqrt(2.0 * std::numbers::pi / t_years) * (p - intrinsic) / (0.5 * (forward + strike));
  if (!(sigma > lo && sigma < hi)) sigma = 0.5;  // NOLINT(readability-simplify-boolean-expr): NaN
  const double sqrt_t = std::sqrt(t_years);
  for (int i = 1; i <= max_iterations; ++i) {
    r.iterations = i;
    const Greeks g = black76(cp, forward, strike, t_years, sigma);
    const double diff = g.price - p;
    if (std::fabs(diff) <= tol) {
      r.vol = sigma;
      r.status = IvStatus::Ok;
      return r;
    }
    if (diff > 0.0) {
      hi = sigma;
    } else {
      lo = sigma;
    }
    if (hi - lo <= 1e-15 * hi) {
      r.vol = 0.5 * (lo + hi);
      r.status = IvStatus::Ok;
      return r;
    }
    const double vega =
        forward * norm_pdf(std::log(forward / strike) / (sigma * sqrt_t) + 0.5 * sigma * sqrt_t) *
        sqrt_t;
    double next = vega > 1e-300 ? sigma - diff / vega : 0.0;
    // Newton left the bracket (or produced NaN): bisect.
    if (!(next > lo && next < hi))
      next = 0.5 * (lo + hi);  // NOLINT(readability-simplify-boolean-expr)
    sigma = next;
  }
  r.vol = 0.5 * (lo + hi);
  r.status = IvStatus::NoConvergence;
  return r;
}

// ---- inverse (coin-margined) conversions ----------------------------------------------------

// Quote-currency value -> price in the base coin (Deribit BTC options: USD / underlying price).
[[nodiscard]] inline double to_coin_price(double quote_value, double forward) noexcept {
  return forward > 0.0 ? quote_value / forward : 0.0;
}
// Delta of a coin-quoted option in coin units. With V_coin = V / F, the coin value of the
// position changes by F * dV_coin/dF = dV/dF - V / F = delta - V_coin per unit move of F.
[[nodiscard]] constexpr double coin_delta(double delta, double coin_price) noexcept {
  return delta - coin_price;
}

}  // namespace fastmm::options
