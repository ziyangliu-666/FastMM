// Opt-in, read-only check of Coinbase Advanced Trade with a CDP API key: FASTMM_LIVE_TESTS=1 and
// COINBASE_API_KEY_NAME / COINBASE_API_PRIVATE_KEY exported. It reads the accounts, the open
// orders and the fills over REST with the connector's JWTs, then runs the connector up to its
// start-up sweep (the user channel subscribed, fills replayed, open orders listed) and stops.
// It places, amends and cancels nothing, and never calls cancel_all().
#include "live_test_util.hpp"

#include "fastmm/venues/blocking_http.hpp"
#include "fastmm/venues/coinbase/advanced_venue.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::coinbase;
using namespace fastmm::venues::test;

TEST_CASE("live.coinbase_advanced: read-only accounts, orders, fills and the user channel") {
  const std::string key = env_or_empty("COINBASE_API_KEY_NAME");
  const std::string pem = env_or_empty("COINBASE_API_PRIVATE_KEY");
  if (!live_tests_enabled() || key.empty() || pem.empty()) {
    MESSAGE("skipped: FASTMM_LIVE_TESTS=1, COINBASE_API_KEY_NAME and COINBASE_API_PRIVATE_KEY");
    return;
  }
  VenueSection s;
  s.name = "coinbase-live";
  s.kind = "coinbase_advanced";
  s.ws_url = "wss://advanced-trade-ws.coinbase.com";
  s.rest_url = "https://api.coinbase.com";
  s.testnet = false;
  s.api_key = key;
  s.api_secret = pem;
  s.extra["cancel_on_order_channel_loss"] = "false";  // read-only: no cancel on a drop either
  AdvancedVenueConfig cfg = make_coinbase_advanced_config(s, false);
  // FASTMM_LIVE_RECORD_DIR: the frames of both channels, as --record-raw writes them.
  cfg.record_raw_dir = env_or_empty("FASTMM_LIVE_RECORD_DIR");
  const CdpJwtSigner signer(cfg.credentials);
  REQUIRE(signer.usable());

  // REST with the connector's JWTs.
  BlockingHttpOptions opts;
  opts.timeout_ms = 10'000;
  BlockingHttp http(cfg.rest_url, opts);
  auto get = [&](const std::string& target) {
    const std::string jwt = signer.sign(wall_now().ns / 1'000'000'000,
                                        CdpJwtSigner::rest_uri("GET", "api.coinbase.com", target));
    return http.get(target, "User-Agent: fastmm\r\nAuthorization: Bearer " + jwt + "\r\n");
  };
  const HttpReply accounts = get(std::string(kAdvancedPrefix) + "/accounts");
  REQUIRE_MESSAGE(accounts.status == 200, accounts.error << " " << accounts.body.substr(0, 200));
  std::size_t n_accounts = 0;
  bool more = false;
  REQUIRE(decode_adv_accounts(accounts.body, n_accounts, more).empty());
  MESSAGE("accounts: " << n_accounts << " (has_next " << more << ")");

  const std::vector<std::string> products = {"BTC-USD"};
  const HttpReply open = get(AdvancedOrderEncoder::open_orders_path(products, {}));
  REQUIRE_MESSAGE(open.status == 200, open.body.substr(0, 200));
  std::vector<AdvOrderRow> orders;
  std::string cursor;
  REQUIRE(decode_adv_orders(open.body, orders, cursor, more).empty());
  MESSAGE("open BTC-USD orders: " << orders.size());

  const HttpReply fills = get(AdvancedOrderEncoder::fills_path(products, 0, 0, {}, 100));
  REQUIRE_MESSAGE(fills.status == 200, fills.body.substr(0, 200));
  std::vector<AdvFillRow> rows;
  REQUIRE(decode_adv_fills(fills.body, rows, cursor).empty());
  MESSAGE("BTC-USD fills: " << rows.size());

  // The connector up to its start-up sweep, with no order sent.
  InstrumentTable instruments;
  Instrument btc = make_instrument("BTC-USD", 1, "BTC", "USD");
  REQUIRE(instruments.add(btc));
  CoinbaseAdvancedVenue venue(VenueId{1}, cfg);
  REQUIRE(venue.load_reference_data(instruments));
  SymbolTable symbols;
  REQUIRE(symbols.build(instruments));
  RecordingSink md(64U << 20);
  RecordingSink order_sink(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  venue.attach(symbols, instruments, md.sink, order_sink.sink, &outbound);
  const InstrumentId ids[] = {InstrumentId{0}};
  venue.subscribe(ids);
  net::Reactor reactor;
  venue.connect(reactor);
  Collected oc;
  auto ends = [&] {
    oc.take(order_sink);
    std::size_t e = 0;
    for (const auto& m : oc.all) {
      e += RecordingSink::type_of(m) == EventType::Reconcile &&
                   RecordingSink::as<ReconcileMsg>(m).kind == ReconcileMsg::Kind::End
               ? 1U
               : 0U;
    }
    return e;
  };
  const bool swept = pump_until(
      reactor,
      [&] {
        return ends() >= 1 && venue.order_channel_live() && venue.md_feed()->synced_count() == 1;
      },
      30'000);
  const VenueStatus st = venue.status();
  venue.disconnect();
  reactor.run_once(0);
  CHECK(swept);
  CHECK_FALSE(venue.fatal());
  MESSAGE("sweep: open orders " << oc.count(EventType::Reconcile) - 2 << ", replayed fills "
                                << oc.count(EventType::OrderFill) << ", rest requests "
                                << st.rest_requests << " errors " << st.rest_errors);
  CHECK(st.rest_errors == 0);
}
