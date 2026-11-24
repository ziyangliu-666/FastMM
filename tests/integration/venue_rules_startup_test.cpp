// fastmm-live's venue start-up (make_venue_slots) when the venue's trading rules differ from
// [[instruments]]: [venues.<name>] rules_mismatch adopts the venue's, keeps the configured ones
// with a warning, or refuses to start (exit code 3).
#include "../venues/fake_venue_util.hpp"

#include "fastmm/live/session.hpp"
#include "fastmm/live/venue_slot.hpp"

#include <string>

using namespace fastmm;
using namespace fastmm::venues::test;

namespace {

struct Start {
  int rc = -1;
  InstrumentTable instruments;
};

// The fixture's BTCUSDT: tick 0.01, lot 0.00001, min notional 5. The configuration says lot 0.001
// and sets no min_notional.
Start start(const char* rules_mismatch) {
  FakeVenueServer srv;
  const std::string info = fastmm::test::fixture("binance/exchange_info.json");
  srv.route("GET", "/api/v3/exchangeInfo", [&](const net::HttpRequest&) {
    return net::HttpServerResponse::json(200, info);
  });
  srv.start();
  Config cfg;
  VenueSection s;
  s.name = "binance";
  s.kind = "binance";
  s.ws_url = srv.ws_base() + "/stream";
  s.ws_api_url = srv.ws_base() + "/ws-api/v3";
  s.rest_url = srv.http_base();
  s.api_key = "fake-key";
  s.api_secret = "fake-secret";
  s.rules_mismatch = rules_mismatch;
  cfg.venues.push_back(s);
  InstrumentSection i;
  i.venue = "binance";
  i.symbol = "BTCUSDT";
  i.base = "BTC";
  i.quote = "USDT";
  i.tick = "0.01";
  i.lot = "0.001";
  cfg.instruments.push_back(i);
  Start out;
  out.instruments = load_instruments(cfg);
  live::VenueSlots slots;
  out.rc = live::make_venue_slots(cfg, {}, out.instruments, "test", slots);
  srv.stop();
  return out;
}

}  // namespace

TEST_CASE("live.rules: a configured lot the venue does not use is adopted, kept or refused") {
  const Qty venue_lot = Qty::from_decimal("0.00001").value();
  const Qty conf_lot = Qty::from_decimal("0.001").value();
  const Notional venue_min = Notional::from_int(5);

  const Start adopt = start("adopt");
  CHECK(adopt.rc == 0);
  CHECK(adopt.instruments.get(InstrumentId{0}).lot == venue_lot);
  CHECK(adopt.instruments.get(InstrumentId{0}).min_notional == venue_min);

  const Start warn = start("warn");
  CHECK(warn.rc == 0);
  CHECK(warn.instruments.get(InstrumentId{0}).lot == conf_lot);
  // Not set in the configuration: the venue's, whatever rules_mismatch says.
  CHECK(warn.instruments.get(InstrumentId{0}).min_notional == venue_min);

  CHECK(start("refuse").rc == live::kExitConfig);
}

TEST_CASE("live.rules: settle_venue_rules names each field that differs") {
  Config cfg;
  VenueSection s;
  s.name = "binance";
  s.kind = "binance";
  s.rules_mismatch = "refuse";
  cfg.venues.push_back(s);
  InstrumentSection i;
  i.venue = "binance";
  i.symbol = "BTCUSDT";
  i.tick = "0.01";
  i.lot = "0.0001";
  i.min_notional = "5";
  cfg.instruments.push_back(i);
  const InstrumentTable configured = load_instruments(cfg);
  InstrumentTable loaded = configured;
  Instrument& now = loaded.get(InstrumentId{0});
  CHECK(live::settle_venue_rules(cfg, configured, loaded).empty());  // nothing differs
  now.lot = Qty::from_decimal("0.01").value();
  now.min_notional = Notional::from_int(10);
  now.max_qty = Qty::from_int(100);  // not configured: not compared
  const std::string err = live::settle_venue_rules(cfg, configured, loaded);
  CHECK(err.find("venues.binance: BTCUSDT lot is 0.0001 in [[instruments]], 0.01 on the venue") !=
        std::string::npos);
  CHECK(err.find("BTCUSDT min_notional is 5 in [[instruments]], 10 on the venue") !=
        std::string::npos);
  CHECK(err.find("max_qty") == std::string::npos);
  CHECK(err.find("tick") == std::string::npos);
}
