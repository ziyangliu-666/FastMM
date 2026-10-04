// The venue registry (include/fastmm/venues/registry.hpp): `kind` resolution, the capabilities a
// connector declares, and the config keys it owns. A venue outside the tree registers the same
// way; tests/venues/external_venue_test.cpp builds the one in examples/external-venue.
#include "fastmm/venues/registry.hpp"

#include "test_support.hpp"

#include "fastmm/config/config.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;

namespace {

VenueRegistry builtins() {
  VenueRegistry r;
  register_builtin_venues(r);
  return r;
}

VenueSection section(std::string kind) {
  VenueSection v;
  v.name = "v";
  v.kind = std::move(kind);
  return v;
}

}  // namespace

TEST_CASE("venues.registry: every shipped connector resolves through its kind and aliases") {
  const VenueRegistry r = builtins();
  CHECK(r.entries().size() == 10);
  for (const char* kind : {"binance_spot",
                           "binance_usdm",
                           "bybit",
                           "coinbase_advanced",
                           "coinbase_exchange",
                           "deribit",
                           "gate_usdt",
                           "gemini",
                           "nasdaq_itch",
                           "okx"})
    CHECK(r.find(kind) != nullptr);
  // Aliases select the same entry, so `kind = "sim"` is the Binance connector.
  CHECK(r.find("sim") == r.find("binance_spot"));
  CHECK(r.find("binance") == r.find("binance_spot"));
  CHECK(r.find("bybit_spot") == r.find("bybit"));
  CHECK(r.find("gate") == r.find("gate_usdt"));
  CHECK(r.find("nope") == nullptr);
  CHECK(r.kinds().find("nasdaq_itch") != std::string::npos);
}

TEST_CASE("venues.registry: a connector declares whether it needs credentials") {
  const VenueRegistry r = builtins();
  // The one special case session.cpp used to spell out as `kind == NasdaqItch`.
  CHECK(r.find("nasdaq_itch")->caps.credentials == false);
  CHECK(r.find("nasdaq_itch")->caps.polls == true);
  CHECK(r.find("nasdaq_itch")->caps.positions == false);
  for (const char* kind : {"binance_spot", "binance_usdm", "bybit", "deribit", "okx"}) {
    CAPTURE(kind);
    CHECK(r.find(kind)->caps.credentials);
    CHECK(r.find(kind)->caps.order_entry);
    CHECK(r.find(kind)->caps.replace);
    CHECK(r.find(kind)->caps.positions);
    CHECK_FALSE(r.find(kind)->caps.polls);
  }
  // Gemini has no amend.
  CHECK(r.find("gemini")->caps.credentials);
  CHECK(r.find("gemini")->caps.order_entry);
  CHECK(r.find("gemini")->caps.positions);
  CHECK_FALSE(r.find("gemini")->caps.replace);
  // Coinbase spot, both APIs: REST orders with no replace, and no positions to report.
  for (const char* kind : {"coinbase_advanced", "coinbase_exchange"}) {
    CAPTURE(kind);
    const VenueEntry* cb = r.find(kind);
    CHECK(cb->caps.credentials);
    CHECK(cb->caps.order_entry);
    CHECK(cb->caps.executions);
    CHECK_FALSE(cb->caps.replace);
    CHECK_FALSE(cb->caps.positions);
  }
}

TEST_CASE("venues.registry: a venue validates the keys it owns") {
  const Config cfg = Config::parse(R"(
[venues.b]
kind = "binance_spot"
stale_ms = 10000
order_api = "rest"
not_a_real_key = 1
)");
  const VenueRegistry r = builtins();
  // A key the connector does not have stops the session, with the line.
  try {
    validate_venues(cfg, r);
    FAIL("an unknown venue key passed");
  } catch (const ConfigError& e) {
    CHECK(std::string(e.what()) ==
          "unknown key 'venues.b.not_a_real_key' (the binance_spot connector does not have it) "
          "(line 6:1)");
    CHECK(e.line() == 6);
  }
  const Config fine = Config::parse(
      "[venues.b]\nkind = \"binance_spot\"\nstale_ms = 10000\norder_api = \"rest\"\n");
  CHECK_NOTHROW(validate_venues(fine, r));

  // A key of the wrong type stops the session, with the line, as the central schema does.
  const Config bad = Config::parse("[venues.b]\nkind = \"binance_spot\"\nstale_ms = \"soon\"\n");
  CHECK_THROWS_WITH_AS(
      validate_venues(bad, r),
      doctest::Contains("'venues.b.stale_ms' has the wrong type (expected integer)"),
      ConfigError);

  // A key another connector owns is unknown here.
  const Config other = Config::parse("[venues.b]\nkind = \"bybit\"\nmd_format = \"sbe\"\n");
  CHECK_THROWS_WITH_AS(
      validate_venues(other, r), doctest::Contains("venues.b.md_format"), ConfigError);

  // An unknown kind names the ones that are registered.
  const Config unknown = Config::parse("[venues.b]\nkind = \"nope\"\n");
  CHECK_THROWS_WITH_AS(validate_venues(unknown, r),
                       doctest::Contains("unsupported kind 'nope' (registered: binance_spot"),
                       std::invalid_argument);
}

TEST_CASE("venues.registry: a pool member needs a connector that runs as one") {
  VenueRegistry r;
  const VenueEntry::Factory make = [](VenueId, const VenueSection&, const VenueFactoryOptions&) {
    return std::unique_ptr<Venue>{};
  };
  VenueEntry pooled{.name = "pooled", .summary = "runs as a member", .make = make};
  pooled.caps.account_pools = true;
  REQUIRE(r.add(pooled) == AddResult::Added);
  REQUIRE(r.add({.name = "alone", .summary = "does not", .make = make}) == AddResult::Added);
  const Config ok = Config::parse(
      "[venues.a]\nkind = \"pooled\"\n[venues.b]\nkind = \"pooled\"\npool_of = \"a\"\n");
  CHECK_NOTHROW(validate_venues(ok, r));
  const Config no = Config::parse(
      "[venues.a]\nkind = \"alone\"\n[venues.b]\nkind = \"alone\"\npool_of = \"a\"\n");
  CHECK_THROWS_WITH_AS(validate_venues(no, r),
                       doctest::Contains("venues.b.pool_of: connector 'alone' does not run as a "
                                         "pool member"),
                       ConfigError);
}

TEST_CASE("venues.registry: adding an entry twice, and a name another entry claims") {
  VenueRegistry r;
  static constexpr VenueKeySpec kKeys[] = {{"greeting", KeyType::String, false, "a greeting"}};
  static constexpr std::string_view kAliases[] = {"echo_v1"};
  const VenueEntry::Factory make = [](VenueId, const VenueSection&, const VenueFactoryOptions&) {
    return std::unique_ptr<Venue>{};
  };
  const VenueEntry entry{.name = "echo",
                         .summary = "an echo venue",
                         .aliases = kAliases,
                         .keys = kKeys,
                         .caps = {},
                         .make = make};
  CHECK(r.add(entry) == AddResult::Added);
  CHECK(r.add(entry) == AddResult::AlreadyPresent);
  VenueEntry other = entry;
  other.make = [](VenueId, const VenueSection&, const VenueFactoryOptions&) {
    return std::unique_ptr<Venue>{};
  };
  CHECK(r.add(other) == AddResult::Conflict);
  other.aliases = {};
  other.name = "echo_v1";  // already an alias of the first entry
  CHECK(r.add(other) == AddResult::Conflict);
  other.name = "";
  CHECK(r.add(other) == AddResult::Invalid);
  // An alias that repeats the entry's own name is a mistake, not a registration.
  VenueEntry self_alias = entry;
  self_alias.name = "echo_v2";
  static constexpr std::string_view kSelf[] = {"echo_v2"};
  self_alias.aliases = kSelf;
  CHECK(r.add(self_alias) == AddResult::Invalid);
  CHECK(r.entries().size() == 1);
}

TEST_CASE("venues.registry: make_venue builds the connector its kind names") {
  const VenueRegistry r = builtins();
  VenueSection s = section("sim");
  s.ws_url = "ws://127.0.0.1:9080/stream";
  s.rest_url = "http://127.0.0.1:9080";
  VenueFactoryOptions opts;
  opts.dry_run = true;
  const std::unique_ptr<Venue> v = make_venue(VenueId{0}, s, opts, r);
  REQUIRE(v != nullptr);
  CHECK(v->name() == "v");
  CHECK_THROWS_AS(static_cast<void>(make_venue(VenueId{0}, section("nope"), opts, r)),
                  std::invalid_argument);
}

TEST_CASE("venues.registry: stale_ms past dead_ms is refused when the connector is built") {
  // The Connection would refuse it on the venue's thread, where nothing catches the exception.
  const VenueRegistry r = builtins();
  VenueFactoryOptions opts;
  opts.dry_run = true;
  for (const char* kind : {"binance_spot",
                           "binance_usdm",
                           "bybit",
                           "coinbase_advanced",
                           "coinbase_exchange",
                           "deribit",
                           "gemini",
                           "okx"}) {
    INFO(kind);
    VenueSection s = section(kind);
    s.ws_url = "wss://127.0.0.1:9080/stream";
    s.rest_url = "https://127.0.0.1:9080";
    s.extra["stale_ms"] = "60000";
    s.extra["dead_ms"] = "10000";
    CHECK_THROWS_WITH_AS(static_cast<void>(make_venue(VenueId{0}, s, opts, r)),
                         doctest::Contains("require 0 < stale_ms <= dead_ms"),
                         std::invalid_argument);
    s.extra["stale_ms"] = "0";
    CHECK_THROWS_WITH_AS(static_cast<void>(make_venue(VenueId{0}, s, opts, r)),
                         doctest::Contains("require 0 < stale_ms <= dead_ms"),
                         std::invalid_argument);
  }
}
