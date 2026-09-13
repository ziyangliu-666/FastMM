#pragma once
// Opt-in testnet tests: they run only when FASTMM_LIVE_TESTS=1 (and, for order entry, when the
// venue's API keys are exported); otherwise they MESSAGE("skipped: ...") and pass.
#include "fake_venue_util.hpp"

#include <cstdlib>
#include <string>

namespace fastmm::venues::test {

inline bool live_tests_enabled() {
  const char* v = std::getenv("FASTMM_LIVE_TESTS");
  return v != nullptr && std::string(v) == "1";
}
inline std::string env_or_empty(const char* name) {
  const char* v = std::getenv(name);
  return v == nullptr ? std::string{} : std::string(v);
}

// A post-only buy far below the market: price = best_bid * 0.8 on the tick grid, quantity the
// smallest lot multiple that satisfies min_qty and min_notional.
inline OutNewOrderMsg far_passive_buy(const Instrument& inst, Price best_bid, ClientOrderId id) {
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, inst.id, inst.venue);
  n.cl_ord_id = id;
  n.side = Side::Buy;
  n.type = OrderType::PostOnly;
  n.tif = TimeInForce::Gtc;
  const std::int64_t raw = best_bid.raw / 10 * 8;
  n.price = inst.round_price(Price::from_raw(raw), Side::Buy);
  Qty q = inst.min_qty.is_positive() ? inst.min_qty : inst.lot;
  if (inst.min_notional.is_positive() && n.price.is_positive()) {
    // qty >= min_notional / price, rounded up to the lot, plus two lots of margin.
    const double need = inst.min_notional.to_double() / n.price.to_double();
    const auto lots = static_cast<std::int64_t>(need / inst.lot.to_double()) + 2;
    const Qty by_notional = Qty::from_raw(lots * inst.lot.raw);
    if (by_notional > q) q = by_notional;
  }
  n.qty = q;
  return n;
}

}  // namespace fastmm::venues::test
