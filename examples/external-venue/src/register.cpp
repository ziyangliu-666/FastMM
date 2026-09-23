// The whole seam between this connector and FastMM: one registry entry naming the `kind` that
// selects it, what it can do and the `[venues.<name>]` keys it owns. Nothing here is generated and
// nothing in FastMM needs to change to add it.
#include "echo/echo_venue.hpp"

#include <stdexcept>
#include <string>

namespace echo {

using fastmm::KeyType;
using fastmm::VenueSection;
using fastmm::venues::VenueEntry;
using fastmm::venues::VenueFactoryOptions;
using fastmm::venues::VenueKeySpec;
using fastmm::venues::VenueRegistry;

namespace {

// The registry checks these before the factory runs: a key that is not here warns with its line,
// a key of the wrong type stops the session. They are the connector's, not the central schema's.
constexpr VenueKeySpec kEchoKeys[] = {
    {"bid", KeyType::String, false, "bid price of the single quote, decimal (default 100)"},
    {"ask", KeyType::String, false, "ask price of the single quote, decimal (default 101)"},
    {"qty", KeyType::String, false, "quantity on both sides, decimal (default 1)"},
};

std::string get(const VenueSection& s, const char* key) {
  const auto it = s.extra.find(key);
  return it == s.extra.end() ? std::string{} : it->second;
}

template <class T>
T decimal(const VenueSection& s, const char* key, T fallback) {
  const std::string text = get(s, key);
  if (text.empty()) return fallback;
  const auto v = T::from_decimal(text);
  if (!v) throw std::invalid_argument("venues." + s.name + "." + key + ": not a decimal");
  return *v;
}

std::unique_ptr<fastmm::venues::Venue> make(fastmm::VenueId id,
                                            const VenueSection& s,
                                            const VenueFactoryOptions& opts) {
  EchoVenueConfig c;
  c.name = s.name;
  c.dry_run = opts.dry_run;
  c.bid = decimal(s, "bid", c.bid);
  c.ask = decimal(s, "ask", c.ask);
  c.qty = decimal(s, "qty", c.qty);
  if (!(c.bid < c.ask)) throw std::invalid_argument("venues." + s.name + ": bid must be below ask");
  return std::make_unique<EchoVenue>(id, std::move(c));
}

}  // namespace

void register_echo_venue(VenueRegistry& r) {
  static_cast<void>(r.add({.name = "echo",
                           .summary = "a fixed quote and no order entry (example connector)",
                           .keys = kEchoKeys,
                           .caps = {.credentials = false,
                                    .order_entry = false,
                                    .replace = false,
                                    .positions = false,
                                    .polls = false},
                           .make = &make}));
}

}  // namespace echo
