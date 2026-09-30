// OKX demo trading, read-only (opt-in: FASTMM_LIVE_TESTS=1 and FASTMM_OKX_API_KEY /
// FASTMM_OKX_API_SECRET / FASTMM_OKX_API_PASSPHRASE of a demo key; FASTMM_OKX_REGION, default
// "eea", picks the hosts as [venues] region does). No order is placed: the connector starts, its
// start-up reconciliation reads GET /api/v5/account/balance through the balance leg, and the
// account channel's first push arrives. Both must agree on BTC and USDT (nothing trades). The
// balances are printed for comparison with the raw reply. FASTMM_OKX_RECORD_DIR, when set, records
// the private frames (RawRecorder).
#include "live_test_util.hpp"

#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/okx/okx_venue.hpp"

#include <fmt/format.h>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::okx;
using namespace fastmm::venues::test;

TEST_CASE("live.okx: demo balances from the balance leg and the account channel agree") {
  if (!live_tests_enabled()) {
    MESSAGE("skipped: set FASTMM_LIVE_TESTS=1 to run against OKX demo trading");
    return;
  }
  const std::string key = env_or_empty("FASTMM_OKX_API_KEY");
  const std::string secret = env_or_empty("FASTMM_OKX_API_SECRET");
  const std::string pass = env_or_empty("FASTMM_OKX_API_PASSPHRASE");
  if (key.empty() || secret.empty() || pass.empty()) {
    MESSAGE("skipped: FASTMM_OKX_API_KEY / _SECRET / _PASSPHRASE (demo) not set");
    return;
  }
  std::string region = env_or_empty("FASTMM_OKX_REGION");
  if (region.empty()) region = "eea";

  VenueSection s;
  s.name = "okx-demo";
  s.kind = "okx";
  s.testnet = true;
  s.api_key = key;
  s.api_secret = secret;
  s.api_passphrase = pass;
  s.extra["region"] = region;
  s.extra["order_api"] = "rest";        // no order connection: nothing is sent
  s.extra["dead_mans_switch_s"] = "0";  // nothing to protect
  OkxVenueConfig cfg = make_okx_config(s, false);
  cfg.record_raw_dir = env_or_empty("FASTMM_OKX_RECORD_DIR");

  InstrumentTable instruments;
  Instrument spot = make_instrument("BTC-USDT", 0, "BTC", "USDT");
  REQUIRE(instruments.add(spot));
  RecordingSink md(16U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  OkxVenue venue(VenueId{0}, cfg);
  const auto ref = venue.load_reference_data(instruments);
  REQUIRE_MESSAGE(ref, (ref ? std::string() : ref.error()));
  REQUIRE(symbols.build(instruments));
  venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
  const InstrumentId ids[] = {InstrumentId{0}};
  venue.subscribe(ids);
  venue.connect(reactor);

  // The snapshot (kSnapshotEnd) and at least one account-channel push (no kSnapshot).
  Collected oc;
  const auto snapshot_ended = [&] {
    return oc.first_if<BalanceMsg>(EventType::Balance, [](const BalanceMsg& m) {
      return (m.flags & BalanceMsg::kSnapshotEnd) != 0;
    }) != nullptr;
  };
  const auto pushed = [&] {
    return oc.first_if<BalanceMsg>(EventType::Balance, [](const BalanceMsg& m) {
      return (m.flags & BalanceMsg::kSnapshot) == 0;
    }) != nullptr;
  };
  REQUIRE(pump_until(
      reactor,
      [&] {
        oc.take(orders);
        return snapshot_ended() && pushed();
      },
      30'000));

  for (const auto& raw : oc.all) {
    if (RecordingSink::type_of(raw) != EventType::Balance) continue;
    const auto& m = RecordingSink::as<BalanceMsg>(raw);
    MESSAGE(fmt::format("{:<8} {:<5} free {} locked {} total {} equity {} maint {} exch_ms {}",
                        (m.flags & BalanceMsg::kSnapshot) != 0 ? "snapshot" : "push",
                        m.asset.view(),
                        DecimalText(m.free).view(),
                        DecimalText(m.locked).view(),
                        DecimalText(m.total).view(),
                        DecimalText(m.equity).view(),
                        DecimalText(m.maintenance).view(),
                        m.hdr.exch_ts.ns / 1'000'000));
    CHECK(m.hdr.exch_ts.ns > 0);
    CHECK((m.flags & BalanceMsg::kAccount) == 0);  // the demo account is in spot mode
  }
  for (const char* asset : {"BTC", "USDT"}) {
    const auto named = [asset](std::uint8_t want) {
      return [asset, want](const BalanceMsg& m) {
        return m.asset.view() == asset && (m.flags & BalanceMsg::kSnapshot) == want;
      };
    };
    const BalanceMsg* rest =
        oc.last_if<BalanceMsg>(EventType::Balance, named(BalanceMsg::kSnapshot));
    const BalanceMsg* push = oc.last_if<BalanceMsg>(EventType::Balance, named(0));
    REQUIRE(rest != nullptr);
    if (push == nullptr) continue;  // not pushed: no balance in it
    CHECK(rest->free == push->free);
    CHECK(rest->locked == push->locked);
    CHECK(rest->total == push->total);
  }
  CHECK_FALSE(venue.fatal());
  venue.disconnect();
}
