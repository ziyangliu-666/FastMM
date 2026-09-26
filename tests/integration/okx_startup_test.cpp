// fastmm-live's venue start-up (make_venue_slots) with an OKX venue whose account is in long/short
// position mode: the connector refuses it, and the exit code is 3 (a setting to fix), not 4
// (reference data that may load on a retry).
#include "../venues/fake_venue_util.hpp"

#include "fastmm/live/session.hpp"
#include "fastmm/live/venue_slot.hpp"

#include <cstdlib>
#include <string>

using namespace fastmm;
using namespace fastmm::venues::test;

namespace {

int start(const std::string& account_config) {
  FakeVenueServer srv;
  const std::string info = fastmm::test::fixture("okx/instruments_btc_usdt_swap.json");
  const std::string time = fastmm::test::fixture("okx/server_time.json");
  srv.route("GET", "/api/v5/public/time", [&](const net::HttpRequest&) {
    return net::HttpServerResponse::json(200, time);
  });
  srv.route("GET", "/api/v5/public/instruments", [&](const net::HttpRequest&) {
    return net::HttpServerResponse::json(200, info);
  });
  srv.route("GET", "/api/v5/account/config", [&](const net::HttpRequest&) {
    return net::HttpServerResponse::json(200, account_config);
  });
  srv.start();
  Config cfg;
  VenueSection s;
  s.name = "okx";
  s.kind = "okx";
  s.ws_url = srv.ws_base() + "/ws/v5/public";
  s.rest_url = srv.http_base();
  s.api_key = "fake-key";
  s.api_secret = "fake-secret";
  s.api_passphrase = "fake-pass";
  cfg.venues.push_back(s);
  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BTC-USDT-SWAP", 0, "BTC", "USDT")));
  live::VenueSlots slots;
  const int rc = live::make_venue_slots(cfg, {}, instruments, "test", slots);
  srv.stop();
  return rc;
}

}  // namespace

TEST_CASE("okx.startup: a dry run clears an unset api_passphrase, a keyed run needs it") {
  unsetenv("FASTMM_T_OKX_UNSET_PASS");
  Config cfg;
  VenueSection s;
  s.name = "okx";
  s.kind = "okx";
  s.api_key = "k";
  s.api_secret = "s";
  s.api_passphrase = "${FASTMM_T_OKX_UNSET_PASS}";
  cfg.venues.push_back(s);
  Config dry = cfg;
  CHECK(live::resolve_venue_env(dry, true, "test"));
  CHECK(dry.venues[0].api_passphrase.empty());
  Config keyed = cfg;
  CHECK_FALSE(live::resolve_venue_env(keyed, false, "test"));
  setenv("FASTMM_T_OKX_UNSET_PASS", "p", 1);
  CHECK(live::resolve_venue_env(keyed, false, "test"));
  CHECK(keyed.venues[0].api_passphrase == "p");
  unsetenv("FASTMM_T_OKX_UNSET_PASS");
}

TEST_CASE("okx.startup: long/short position mode exits 3, net mode starts") {
  CHECK(start(R"({"code":"0","msg":"","data":[{"acctLv":"2","posMode":"long_short_mode"}]})") ==
        live::kExitConfig);
  CHECK(start(R"({"code":"0","msg":"","data":[{"acctLv":"2","posMode":"net_mode"}]})") == 0);
}
