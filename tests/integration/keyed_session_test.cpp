// Which sessions fastmm-live holds to [risk] position and loss limits (live::trades_with_keys): a
// venue that trades with an API key or an OUCH login, not a public_only one, not a dry run (whose
// keys resolve_venue_env has cleared).
#include "test_support.hpp"

#include "fastmm/live/session.hpp"

#include <string>

using namespace fastmm;

namespace {

Config parse(const std::string& venue) {
  Config::LoadOptions opts;
  opts.substitute_env = false;
  return Config::parse(venue + R"(
[[instruments]]
venue = "v"
symbol = "BTCUSDT"
tick = "0.01"
lot = "0.00001"
)",
                       opts);
}

}  // namespace

TEST_CASE("integration.live: trades_with_keys is a venue with keys that can trade") {
  const std::string binance = "[venues.v]\nkind = \"binance_spot\"\n";
  CHECK(live::trades_with_keys(parse(binance + "api_key = \"k\"\napi_secret = \"s\"\n")));
  CHECK_FALSE(live::trades_with_keys(parse(binance)));
  CHECK_FALSE(live::trades_with_keys(
      parse(binance + "api_key = \"k\"\napi_secret = \"s\"\npublic_only = true\n")));

  // A dry run's keys are cleared before the check: the same config is not keyed then.
  Config dry = parse(binance + "api_key = \"k\"\napi_secret = \"s\"\n");
  REQUIRE(live::resolve_venue_env(dry, /*dry_run=*/true, "test"));
  CHECK_FALSE(live::trades_with_keys(dry));

  // nasdaq_itch takes no API key; an OUCH login is what trades.
  const std::string itch = "[venues.v]\nkind = \"nasdaq_itch\"\n";
  CHECK_FALSE(live::trades_with_keys(parse(itch)));
  CHECK(live::trades_with_keys(
      parse(itch + "order_entry = \"sim_ouch\"\nouch_username = \"u\"\nouch_password = \"p\"\n")));
  CHECK_FALSE(live::trades_with_keys(
      parse(itch + "order_entry = \"none\"\nouch_username = \"u\"\nouch_password = \"p\"\n")));
}
