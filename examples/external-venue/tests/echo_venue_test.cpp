// What a venue project should check without a network: that the connector registers, that it owns
// its configuration keys, and that the factory builds it. No test framework needed.
#include "echo/echo_venue.hpp"

#include <cstdio>
#include <cstdlib>
#include <fastmm/config/config.hpp>
#include <fastmm/venues/registry.hpp>
#include <string>
#include <vector>

using fastmm::Config;
using fastmm::ConfigError;
using fastmm::VenueId;
using namespace fastmm::venues;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

constexpr const char* kConfig = R"(
[venues.e]
kind = "echo"
bid = "99.5"
ask = "100.5"
typo = 1

[[instruments]]
venue = "e"
symbol = "ECHO"
tick = "0.5"
lot = "1"
)";

}  // namespace

int main() {
  VenueRegistry registry;
  echo::register_echo_venue(registry);
  const VenueEntry* entry = registry.find("echo");
  check(entry != nullptr, "the echo venue is registered");
  if (entry == nullptr) return 1;
  check(!entry->caps.credentials, "the echo venue needs no API keys");
  check(!entry->caps.order_entry, "the echo venue declares no order entry");
  check(entry->keys.size() == 3, "the echo venue owns three keys");

  const Config cfg = Config::parse(kConfig);
  check(cfg.warnings.empty(), "the central schema has nothing to say about the connector's keys");

  // The venue's own keys are validated by the registry: `typo` is not one of them.
  std::vector<std::string> warnings;
  validate_venues(cfg, warnings, registry);
  check(warnings.size() == 1, "one unknown key");
  check(!warnings.empty() && warnings[0].find("venues.e.typo") != std::string::npos,
        "the warning names the key");
  check(!warnings.empty() && warnings[0].find("line 6") != std::string::npos,
        "the warning carries the line");

  const std::unique_ptr<Venue> v = make_venue(VenueId{0}, cfg.venues.at(0), {}, registry);
  check(v != nullptr && v->name() == "e", "make_venue builds the connector");

  // A value the connector refuses is the connector's error, not the schema's.
  const Config crossed = Config::parse("[venues.e]\nkind = \"echo\"\nbid = \"2\"\nask = \"1\"\n");
  bool threw = false;
  try {
    static_cast<void>(make_venue(VenueId{0}, crossed.venues.at(0), {}, registry));
  } catch (const std::exception&) {
    threw = true;
  }
  check(threw, "bid above ask is refused");

  if (failures == 0) std::puts("echo_venue_test: OK");
  return failures == 0 ? 0 : 1;
}
