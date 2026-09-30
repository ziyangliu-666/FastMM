// BinanceVenue's balances against the simulator: the start-up snapshot (account.status,
// omitZeroBalances) and the outboundAccountPosition an order causes report what the simulator's
// account holds.
#include "integration_util.hpp"

#include <vector>

using namespace fastmm;
using namespace fastmm::integration;

namespace {

std::vector<BalanceMsg> balances_of(const Collected& c) {
  std::vector<BalanceMsg> out;
  for (const auto& m : c.all) {
    if (Collected::type_of(m) == EventType::Balance) out.push_back(Collected::as<BalanceMsg>(m));
  }
  return out;
}

std::size_t snapshot_ends(const Collected& c) {
  std::size_t n = 0;
  for (const BalanceMsg& b : balances_of(c)) n += (b.flags & BalanceMsg::kSnapshotEnd) != 0 ? 1 : 0;
  return n;
}

// The last row for `asset`, of a snapshot (`snapshot`) or of the stream.
const BalanceMsg* last_row(const std::vector<BalanceMsg>& rows,
                           std::string_view asset,
                           bool snapshot) {
  const BalanceMsg* out = nullptr;
  for (const BalanceMsg& b : rows) {
    if (b.asset.view() == asset && ((b.flags & BalanceMsg::kSnapshot) != 0) == snapshot) out = &b;
  }
  return out;
}

Qty amount(const char* s) {
  return Qty::from_decimal(s).value();
}

}  // namespace

TEST_CASE("sim_exchange: BinanceVenue reports the simulator's balances") {
  sim::server::SimServerConfig cfg = test_server_config();
  cfg.balances = {{"BTC", amount("2.5")}, {"USDT", amount("150000")}, {"ETH", amount("7")}};
  ServerFixture fx(cfg);
  VenueHarness h(venue_config(fx));
  REQUIRE(h.venue->load_reference_data(h.instruments));
  h.connect();
  REQUIRE(h.pump([&] {
    return h.venue->md_feed()->synced_count() == 1 && h.live_order_channels() >= 2 &&
           snapshot_ends(h.oc) == 1;
  }));
  // The start-up snapshot: BTC and USDT as configured, ETH not kept (no instrument names it).
  std::vector<BalanceMsg> rows = balances_of(h.oc);
  REQUIRE(rows.size() == 2);
  const BalanceMsg* btc = last_row(rows, "BTC", true);
  const BalanceMsg* usdt = last_row(rows, "USDT", true);
  REQUIRE(btc != nullptr);
  REQUIRE(usdt != nullptr);
  CHECK(btc->free == Notional::from_decimal("2.5").value());
  CHECK(btc->locked.is_zero());
  CHECK(usdt->free == Notional::from_int(150000));
  CHECK(usdt->locked.is_zero());
  CHECK((rows.back().flags & BalanceMsg::kSnapshotEnd) != 0);
  CHECK(rows.back().hdr.exch_ts.ns > 0);

  // A resting buy locks its notional in USDT; the simulator's outboundAccountPosition says so.
  const Price bid = fx.server.stats().best_bid.price;
  REQUIRE(bid.is_positive());
  const Price px = bid - Price::from_int(100);
  const Qty qty = Qty::from_decimal("0.01").value();
  const ClientOrderId o1 = cid(1);
  h.send(new_order(o1, Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, px, qty).hdr);
  REQUIRE(h.pump([&] {
    const std::vector<BalanceMsg> r = balances_of(h.oc);
    const BalanceMsg* u = last_row(r, "USDT", false);
    return h.acks(o1) >= 1 && u != nullptr && u->locked.is_positive();
  }));
  rows = balances_of(h.oc);
  const BalanceMsg stream_usdt = *last_row(rows, "USDT", false);
  CHECK(stream_usdt.flags == 0);
  CHECK(stream_usdt.locked == h.instruments.get(InstrumentId{0}).notional(px, qty));
  CHECK(stream_usdt.free + stream_usdt.locked == Notional::from_int(150000));
  CHECK(stream_usdt.hdr.exch_ts.ns > 0);

  // The simulator's own account, asked again (a reconciliation's balance leg): the same amounts.
  h.venue->request_open_orders();
  REQUIRE(h.pump([&] { return snapshot_ends(h.oc) == 2; }));
  rows = balances_of(h.oc);
  const BalanceMsg* snap_usdt = last_row(rows, "USDT", true);
  const BalanceMsg* snap_btc = last_row(rows, "BTC", true);
  REQUIRE(snap_usdt != nullptr);
  REQUIRE(snap_btc != nullptr);
  CHECK(snap_usdt->free == stream_usdt.free);
  CHECK(snap_usdt->locked == stream_usdt.locked);
  CHECK(snap_btc->free == Notional::from_decimal("2.5").value());
  // Stamped with the connector's venue clock (local clock plus the offset measured from
  // GET /api/v3/time), which can be a few milliseconds off the simulator's own stamp u.
  CHECK(snap_usdt->hdr.exch_ts.ns >= stream_usdt.hdr.exch_ts.ns - 100'000'000);
}
