// Gemini perpetuals and spot: the registry entry, the keys the connector owns and its factory.
#include "fastmm/venues/gemini/gemini_venue.hpp"
#include "fastmm/venues/registry.hpp"

namespace fastmm::venues {

namespace {

constexpr VenueKeySpec kGeminiKeys[] = {
    {"cancel_on_disconnect",
     KeyType::Bool,
     false,
     "cancelOnDisconnect=true on the order connection: the venue cancels the orders placed on it "
     "when it closes (default true)"},
    {"heartbeat",
     KeyType::Bool,
     false,
     "the API key has \"Requires Heartbeat\": POST /v1/heartbeat every 15 s; the venue cancels "
     "the key's orders after 30 s without one (default false)"},
    {"stale_ms",
     KeyType::Int,
     false,
     "no traffic for this long marks the feed stale and pulls the venue's quotes, ms (default "
     "2000)"},
    {"dead_ms",
     KeyType::Int,
     false,
     "no traffic for this long forces a reconnect, ms; raised to at least twice ping_interval_ms "
     "plus 5000"},
    {"ping_interval_ms",
     KeyType::Int,
     false,
     "`ping` interval on both connections, ms, 1000 to 60000 (default 10000; the venue sends no "
     "heartbeats)"},
    {"orders_per_second",
     KeyType::Int,
     false,
     "client-side cap on new orders, per second (default 20)"},
    {"allow_offline_reference_data",
     KeyType::Bool,
     false,
     "start without REST reference data, using the configured tick and lot (default false)"},
    {"cancel_on_order_channel_loss",
     KeyType::Bool,
     false,
     "cancel this key's orders over REST (order/cancel/session) when the order connection drops "
     "(default true)"},
};

std::unique_ptr<Venue> make(VenueId id, const VenueSection& s, const VenueFactoryOptions& opts) {
  gemini::GeminiVenueConfig c = gemini::make_gemini_config(s, opts.dry_run);
  c.record_raw_dir = opts.record_raw_dir;
  c.source = source_address(s);
  return std::make_unique<gemini::GeminiVenue>(id, std::move(c));
}

}  // namespace

void register_gemini_venue(VenueRegistry& r) {
  static_cast<void>(r.add({.name = "gemini",
                           .summary = "Gemini perpetuals and spot (sandbox)",
                           .keys = kGeminiKeys,
                           .caps = {.credentials = true,
                                    .order_entry = true,
                                    .replace = false,
                                    // POST /v1/positions
                                    .positions = true,
                                    .polls = false,
                                    // POST /v1/mytrades
                                    .executions = true,
                                    .bind_source = true},
                           .make = &make}));
}

}  // namespace fastmm::venues
