// fastmm-live's venue start-up (make_venue_slots) with a Bybit linear venue whose symbol is in
// hedge mode: the connector refuses it, and the exit code is 3 (a setting to fix), not 4 (reference
// data that may load on a retry).
#include "../venues/fake_venue_util.hpp"

#include "fastmm/live/session.hpp"
#include "fastmm/live/venue_slot.hpp"

#include <string>

using namespace fastmm;
using namespace fastmm::venues::test;

namespace {

int start(const char* position_list_fixture) {
  FakeVenueServer srv;
  const std::string info = fastmm::test::fixture("bybit/linear_instruments_info.json");
  const std::string time = fastmm::test::fixture("bybit/server_time.json");
  const std::string positions = fastmm::test::fixture(position_list_fixture);
  srv.route("GET", "/v5/market/time", [&](const net::HttpRequest&) {
    return net::HttpServerResponse::json(200, time);
  });
  srv.route("GET", "/v5/market/instruments-info", [&](const net::HttpRequest&) {
    return net::HttpServerResponse::json(200, info);
  });
  srv.route("GET", "/v5/position/list", [&](const net::HttpRequest&) {
    return net::HttpServerResponse::json(200, positions);
  });
  srv.start();
  Config cfg;
  VenueSection s;
  s.name = "bybit-linear";
  s.kind = "bybit";
  s.ws_url = srv.ws_base() + "/v5/public/linear";
  s.rest_url = srv.http_base();
  s.api_key = "fake-key";
  s.api_secret = "fake-secret";
  s.extra["category"] = "linear";
  cfg.venues.push_back(s);
  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
  live::VenueSlots slots;
  const int rc = live::make_venue_slots(cfg, {}, instruments, "test", slots);
  srv.stop();
  return rc;
}

}  // namespace

TEST_CASE("bybit_linear.startup: hedge mode exits 3, one-way mode starts") {
  CHECK(start("bybit/linear_position_list_hedge.json") == live::kExitConfig);
  CHECK(start("bybit/linear_position_list.json") == 0);
}
