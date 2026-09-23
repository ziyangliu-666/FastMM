// A venue from outside the tree: examples/external-venue is a separate CMake project, and its two
// source files are compiled into this binary unchanged. Whatever works here works for a project
// that builds against the installed headers (CI builds it that way too,
// scripts/ci-external-project.sh).
#include "echo/echo_venue.hpp"
#include "test_support.hpp"

#include "fastmm/config/config.hpp"
#include "fastmm/venues/registry.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;

TEST_CASE("venues.external: an out-of-tree venue registers next to the built-in ones") {
  VenueRegistry r;
  register_builtin_venues(r);
  echo::register_echo_venue(r);
  REQUIRE(r.find("echo") != nullptr);
  CHECK(r.entries().size() == 6);
  CHECK(r.find("bybit") != nullptr);  // the built-ins are untouched
  CHECK(r.kinds().find("echo") != std::string::npos);
  // It declares what it can do; the session asks the registry instead of testing `kind`.
  CHECK_FALSE(r.find("echo")->caps.credentials);
  CHECK_FALSE(r.find("echo")->caps.order_entry);
}

TEST_CASE("venues.external: an out-of-tree venue owns its configuration keys") {
  const Config cfg = Config::parse(R"(
[venues.e]
kind = "echo"
bid = "99.5"
ask = "100.5"
typo = 1
)");
  // The central schema knows none of them, so it says nothing either way.
  CHECK(cfg.warnings.empty());
  VenueRegistry r;
  echo::register_echo_venue(r);
  std::vector<std::string> warnings;
  validate_venues(cfg, warnings, r);
  REQUIRE(warnings.size() == 1);
  CHECK(warnings[0] == "unknown key 'venues.e.typo' ignored (line 6)");

  const std::unique_ptr<Venue> v = make_venue(VenueId{0}, cfg.venues.at(0), {}, r);
  REQUIRE(v != nullptr);
  CHECK(v->name() == "e");
  CHECK_FALSE(v->caps().user_stream);
}
