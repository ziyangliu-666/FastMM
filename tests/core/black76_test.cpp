// Black-76 pricing, greeks and implied volatility against known values. Reference values were
// computed independently (Python math.erfc implementation of the same closed forms) and the first
// case is the textbook example from Haug, "The Complete Guide to Option Pricing Formulas" (2nd
// ed.), Black-76: F = K = 19, T = 0.75, r = 10 %, sigma = 28 % -> call = put = 1.7011.
#include "fastmm/core/options/black76.hpp"

#include "test_support.hpp"

#include <cmath>
#include <limits>

using namespace fastmm::options;

namespace {
constexpr double kEps = 1e-9;
}

TEST_CASE("core.black76: Haug example, put-call parity and greeks") {
  const Greeks c = black76(CallPut::Call, 19.0, 19.0, 0.75, 0.28, 0.10);
  const Greeks p = black76(CallPut::Put, 19.0, 19.0, 0.75, 0.28, 0.10);
  CHECK(c.price == doctest::Approx(1.7011).epsilon(1e-4));
  CHECK(c.price == doctest::Approx(1.7010507252).epsilon(kEps));
  CHECK(p.price == doctest::Approx(1.7010507252).epsilon(kEps));
  CHECK(c.delta == doctest::Approx(0.5086362359).epsilon(kEps));
  CHECK(p.delta == doctest::Approx(-0.4191072504).epsilon(kEps));
  CHECK(c.gamma == doctest::Approx(0.0797450347).epsilon(kEps));
  CHECK(c.vega == doctest::Approx(6.0454710790).epsilon(kEps));
  CHECK(c.theta == doctest::Approx(-0.9583828622).epsilon(kEps));
  CHECK(c.rho == doctest::Approx(-1.2757880439).epsilon(kEps));
  // Parity: C - P = e^{-rT} (F - K) = 0 here; delta_c - delta_p = e^{-rT}.
  CHECK(c.delta - p.delta == doctest::Approx(std::exp(-0.10 * 0.75)).epsilon(kEps));
  CHECK(c.gamma == doctest::Approx(p.gamma).epsilon(kEps));
  CHECK(c.vega == doctest::Approx(p.vega).epsilon(kEps));
}

TEST_CASE("core.black76: out of and in the money reference values") {
  const Greeks otm = black76(CallPut::Call, 100.0, 120.0, 0.5, 0.3, 0.0);
  CHECK(otm.price == doctest::Approx(2.503775208732).epsilon(kEps));
  CHECK(otm.delta == doctest::Approx(0.225602975996).epsilon(kEps));
  CHECK(otm.gamma == doctest::Approx(0.014159455312).epsilon(kEps));
  CHECK(otm.vega == doctest::Approx(21.239182968523).epsilon(kEps));
  CHECK(otm.theta == doctest::Approx(-6.371754890557).epsilon(kEps));
  const Greeks itm = black76(CallPut::Put, 100.0, 150.0, 0.25, 0.5, 0.02);
  CHECK(itm.price == doctest::Approx(50.419135607256).epsilon(kEps));
  CHECK(itm.delta == doctest::Approx(-0.928132925524).epsilon(kEps));
  CHECK(itm.gamma == doctest::Approx(0.005179172756).epsilon(kEps));
  CHECK(itm.vega == doctest::Approx(6.473965944442).epsilon(kEps));
  CHECK(itm.theta == doctest::Approx(-5.465583232297).epsilon(kEps));
  CHECK(itm.rho == doctest::Approx(-12.604783901814).epsilon(kEps));
  const Greeks hull = black76(CallPut::Put, 20.0, 20.0, 4.0 / 12.0, 0.25, 0.09);
  CHECK(hull.price == doctest::Approx(1.1166414566).epsilon(kEps));
}

TEST_CASE("core.black76: finite-difference greeks agree with the closed forms") {
  constexpr double f = 76904.4;
  constexpr double k = 77000.0;
  constexpr double t = 0.05;
  constexpr double s = 0.45;
  constexpr double r = 0.03;
  for (const CallPut cp : {CallPut::Call, CallPut::Put}) {
    const Greeks g = black76(cp, f, k, t, s, r);
    const double hf = f * 1e-5;
    const double up = black76_price(cp, f + hf, k, t, s, r);
    const double dn = black76_price(cp, f - hf, k, t, s, r);
    CHECK(g.delta == doctest::Approx((up - dn) / (2 * hf)).epsilon(1e-6));
    CHECK(g.gamma == doctest::Approx((up - 2 * g.price + dn) / (hf * hf)).epsilon(1e-4));
    const double hs = 1e-5;
    CHECK(g.vega == doctest::Approx((black76_price(cp, f, k, t, s + hs, r) -
                                     black76_price(cp, f, k, t, s - hs, r)) /
                                    (2 * hs))
                        .epsilon(1e-6));
    const double ht = 1e-6;  // theta is -dV/dT
    CHECK(g.theta == doctest::Approx(-(black76_price(cp, f, k, t + ht, s, r) -
                                       black76_price(cp, f, k, t - ht, s, r)) /
                                     (2 * ht))
                         .epsilon(1e-5));
    const double hr = 1e-6;
    CHECK(g.rho == doctest::Approx((black76_price(cp, f, k, t, s, r + hr) -
                                    black76_price(cp, f, k, t, s, r - hr)) /
                                   (2 * hr))
                       .epsilon(1e-5));
  }
}

TEST_CASE("core.black76: degenerate inputs") {
  Greeks g = black76(CallPut::Call, 100.0, 90.0, 0.0, 0.3, 0.05);
  CHECK(g.price == doctest::Approx(10.0));
  CHECK(g.delta == doctest::Approx(1.0));
  CHECK(g.gamma == 0.0);
  CHECK(g.vega == 0.0);
  g = black76(CallPut::Put, 100.0, 90.0, 1.0, 0.0);
  CHECK(g.price == 0.0);
  CHECK(g.delta == 0.0);
  g = black76(CallPut::Call, 0.0, 90.0, 1.0, 0.3);
  CHECK(g.price == 0.0);
  g = black76(CallPut::Call, 100.0, -1.0, 1.0, 0.3);
  CHECK(g.price == 0.0);
  CHECK(year_fraction(365LL * 86400 * 1'000'000'000, 0) == doctest::Approx(1.0));
}

TEST_CASE("core.black76: implied volatility round trips and bounds") {
  for (const CallPut cp : {CallPut::Call, CallPut::Put}) {
    for (const double k : {50.0, 80.0, 100.0, 125.0, 200.0}) {
      for (const double vol : {0.05, 0.3, 1.2, 4.0}) {
        for (const double t : {1.0 / 365.0, 0.25, 2.0}) {
          const double px = black76_price(cp, 100.0, k, t, vol, 0.01);
          const IvResult r = implied_vol(cp, px, 100.0, k, t, 0.01, 1e-12);
          if (r.status == IvStatus::AtIntrinsic) {
            // Deep in/out of the money with little time: the price carries no time value to
            // invert (below the kMinVol price plus tolerance).
            CHECK(px <= black76_price(cp, 100.0, k, t, kMinVol, 0.01) + 1e-12);
            continue;
          }
          REQUIRE_MESSAGE(r.ok(),
                          "cp=" << static_cast<int>(cp) << " k=" << k << " vol=" << vol
                                << " t=" << t << " status=" << static_cast<int>(r.status));
          CHECK(r.iterations <= 100);
          CHECK(black76_price(cp, 100.0, k, t, r.vol, 0.01) == doctest::Approx(px).epsilon(1e-9));
          if (px > 1e-6) CHECK(r.vol == doctest::Approx(vol).epsilon(1e-5));
        }
      }
    }
  }
  CHECK(implied_vol(CallPut::Call, 1.0, 100.0, 90.0, 1.0).status ==
        IvStatus::OutOfBounds);  // < intrinsic
  CHECK(implied_vol(CallPut::Call, 100.0, 100.0, 90.0, 1.0).status ==
        IvStatus::OutOfBounds);  // >= F
  CHECK(implied_vol(CallPut::Call, 10.0, 100.0, 90.0, 1e-9).status == IvStatus::AtIntrinsic);
  CHECK(implied_vol(CallPut::Call, 5.0, 100.0, 90.0, 0.0).status == IvStatus::BadInput);
  CHECK(implied_vol(CallPut::Put, std::numeric_limits<double>::quiet_NaN(), 100.0, 90.0, 1.0)
            .status == IvStatus::BadInput);
  const IvResult capped = implied_vol(CallPut::Call, 50.0, 100.0, 100.0, 1.0, 0.0, 1e-12, 3);
  CHECK(capped.iterations <= 3);
}

TEST_CASE("core.black76: Deribit testnet ticker greeks (inverse BTC option)") {
  // tests/fixtures/deribit/ticker_option.json: BTC-15SEP26-77000-C at 2026-09-13T22:15:31.096Z,
  // underlying_price 76904.4, mark_iv 31.2, interest_rate 0, mark_price 0.0069 BTC; Deribit greeks
  // delta 0.47736, gamma 0.00028, vega 18.43602, theta -217.49347, rho 1.31068. Expiry 08:00 UTC.
  constexpr std::int64_t kTs = 1789344931096LL * 1'000'000;
  constexpr std::int64_t kExpiry = 1789459200000LL * 1'000'000;
  const double t = year_fraction(kExpiry, kTs);
  const Greeks g = black76(CallPut::Call, 76904.4, 77000.0, t, 0.312, 0.0);
  CHECK(g.delta == doctest::Approx(0.47736).epsilon(2e-4));
  CHECK(g.gamma == doctest::Approx(0.00028).epsilon(0.02));  // Deribit rounds gamma to 2 digits
  CHECK(g.vega / 100.0 == doctest::Approx(18.43602).epsilon(1e-3));
  CHECK(g.theta / 365.0 == doctest::Approx(-217.49347).epsilon(1e-3));
  // Deribit's rho is the spot-model K T N(d2) / 100, not the Black-76 rho.
  const double d2 =
      (std::log(76904.4 / 77000.0) - 0.5 * 0.312 * 0.312 * t) / (0.312 * std::sqrt(t));
  CHECK(77000.0 * t * norm_cdf(d2) / 100.0 == doctest::Approx(1.31068).epsilon(1e-3));
  const double coin = to_coin_price(g.price, 76904.4);
  CHECK(coin == doctest::Approx(0.0069).epsilon(0.01));  // mark 0.0069 BTC, 4 decimals
  CHECK(coin_delta(g.delta, coin) == doctest::Approx(g.delta - coin));
  const IvResult iv = implied_vol(CallPut::Call, g.price, 76904.4, 77000.0, t);
  REQUIRE(iv.ok());
  CHECK(iv.vol == doctest::Approx(0.312).epsilon(1e-6));
}
