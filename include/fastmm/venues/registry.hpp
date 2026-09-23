#pragma once
// Venue registry: `[venues.<name>] kind` -> the connector that serves it. An entry carries the
// venue's capabilities, the `[venues.<name>]` keys the venue owns and one factory, so nothing
// about a connector reaches the core: the central schema (include/fastmm/config/schema.hpp) knows
// only the generic keys, and no `if (kind == X)` lives outside a venue's own code.
//
// Nothing registers itself, for the same reason the strategy and storage registries do not (a
// static library's self-registering object is dropped by the linker):
// fastmm::venues::register_builtin_venues() adds the connectors FastMM ships, and an out-of-tree
// venue calls add() before the session starts (examples/external-venue).
//
//   constexpr VenueKeySpec kEchoKeys[] = {
//       {"greeting", KeyType::String, false, "echo: text the venue sends back (default \"hi\")"},
//   };
//   VenueRegistry::instance().add({.name = "echo",
//                                  .summary = "an echo venue",
//                                  .keys = kEchoKeys,
//                                  .caps = {.credentials = false},
//                                  .make = &make_echo_venue});
//
// docs/how-to/venues/add-a-venue.md walks through it; tools/docs_config_ref.py reads the key
// tables out of the registration files so a venue's keys land in the configuration reference.
#include "fastmm/config/config.hpp"
#include "fastmm/config/schema.hpp"
#include "fastmm/venues/venue.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues {

// Runtime knobs every connector honours, passed to the factory. Anything venue-specific is a
// config key the venue owns instead.
struct VenueFactoryOptions {
  bool dry_run = false;        // public market data only: no keys, no orders
  std::string record_raw_dir;  // non-empty: append raw frames to <dir>/<venue>-<channel>.jsonl
  bool busy_poll = false;      // [engine] spin_mode = "busy": Venue::poll() must make progress
};

// What a connector declares about itself before one exists. The session and the engine read these
// instead of testing `kind`. `replace` and `order_entry` say what the connector can do; whether a
// given instance does is Venue::caps() (a dry run, order_entry = "none", missing credentials).
struct VenueCapabilities {
  bool credentials = true;  // needs api_key / api_secret outside a dry run
  bool order_entry = true;  // can send orders at all
  bool replace = false;     // can amend a resting order in place
  bool positions = false;   // reports positions from the venue
  bool polls = false;       // Venue::poll() drives its sockets; the reactor never wakes for them
};

// One `[venues.<name>]` key a venue owns. `doc` is the Meaning column of the configuration
// reference: say what the key does, its unit and its default, as schema.hpp does.
struct VenueKeySpec {
  std::string_view key;
  KeyType type;
  bool required;
  std::string_view doc;
};

struct VenueEntry {
  // Throws std::invalid_argument (a bad URL, a key the connector rejects); the message names the
  // venue. The section's keys have been validated against `keys` before this is called.
  using Factory = std::unique_ptr<Venue> (*)(VenueId,
                                             const VenueSection&,
                                             const VenueFactoryOptions&);

  // Every member has a default, so a registration names only the ones it needs.
  std::string_view name;                           // the `kind` value; outlives the registry
  std::string_view summary;                        // one line for the connector table
  std::span<const std::string_view> aliases = {};  // other `kind` values that select this entry
  std::span<const VenueKeySpec> keys = {};         // the keys it owns, beyond the generic ones
  VenueCapabilities caps = {};
  Factory make = nullptr;
};

enum class AddResult : std::uint8_t { Added, AlreadyPresent, Conflict, Invalid };
[[nodiscard]] constexpr std::string_view to_string(AddResult r) noexcept {
  switch (r) {
    case AddResult::Added:
      return "added";
    case AddResult::AlreadyPresent:
      return "already present";
    case AddResult::Conflict:
      return "conflict";
    case AddResult::Invalid:
      return "invalid";
  }
  return "?";
}

class VenueRegistry {
 public:
  // The registry fastmm-live resolves `kind` through. Not thread-safe for writers: register
  // before starting threads. Tests use a local registry.
  static VenueRegistry& instance();

  // Registering the same factory under the same name again changes nothing (AlreadyPresent);
  // anything that would replace an entry, or a name an entry already claims as an alias, changes
  // nothing and returns Conflict.
  AddResult add(VenueEntry entry);
  // By `kind`: the entry's name or one of its aliases.
  [[nodiscard]] const VenueEntry* find(std::string_view kind) const noexcept;
  [[nodiscard]] const std::vector<VenueEntry>& entries() const noexcept { return entries_; }
  // Every accepted `kind`, names and aliases, comma-separated, for an error message.
  [[nodiscard]] std::string kinds() const;

 private:
  std::vector<VenueEntry> entries_;
};

// Adds the connectors FastMM ships (binance_spot, binance_usdm, bybit, deribit, nasdaq_itch).
// Idempotent; make_venue() and validate_venues() call it for the default registry.
void register_builtin_venues(VenueRegistry& r = VenueRegistry::instance());

// Checks every `[venues.<name>]` section against the key table of the venue its `kind` names, the
// way the central schema checks the generic keys: a key the venue does not own is appended to
// `warnings` with its line, a key of the wrong type or a missing required key throws ConfigError
// with the line. An unknown `kind` throws too. Call it once after loading a configuration;
// fastmm-live does, before it prints the warnings.
void validate_venues(const Config& cfg,
                     std::vector<std::string>& warnings,
                     const VenueRegistry& r = VenueRegistry::instance());

// Builds the connector `section.kind` names. Throws std::invalid_argument for an unknown kind,
// ConfigError for a key of the wrong type and whatever the connector throws for a bad value.
std::unique_ptr<Venue> make_venue(VenueId id,
                                  const VenueSection& section,
                                  const VenueFactoryOptions& opts);
std::unique_ptr<Venue> make_venue(VenueId id,
                                  const VenueSection& section,
                                  const VenueFactoryOptions& opts,
                                  const VenueRegistry& r);

}  // namespace fastmm::venues
