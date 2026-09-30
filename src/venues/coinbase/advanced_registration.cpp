// Coinbase Advanced Trade spot: the registry entry, the keys the connector owns and its factory.
#include "fastmm/venues/coinbase/advanced_venue.hpp"
#include "fastmm/venues/registry.hpp"

namespace fastmm::venues {

namespace {

constexpr VenueKeySpec kAdvancedKeys[] = {
    {"ws_private_url",
     KeyType::String,
     false,
     "WebSocket URL of the user channel (order states); empty = "
     "wss://advanced-trade-ws-user.coinbase.com"},
    {"stale_ms",
     KeyType::Int,
     false,
     "no traffic for this long marks the feed stale and pulls the venue's quotes, ms (default "
     "2000)"},
    {"dead_ms",
     KeyType::Int,
     false,
     "no traffic for this long forces a reconnect, ms (default 10000; heartbeats come every "
     "second)"},
    {"orders_per_second",
     KeyType::Int,
     false,
     "client-side cap on new orders, per second (default 8)"},
    {"cancel_batch",
     KeyType::Int,
     false,
     "orders one batch_cancel request names in cancel-all, 1 to 100 (default 50)"},
    {"allow_offline_reference_data",
     KeyType::Bool,
     false,
     "start without REST reference data, using the configured tick and lot (default false)"},
    {"cancel_on_order_channel_loss",
     KeyType::Bool,
     false,
     "cancel all orders over REST when the user channel drops (default true)"},
    {"emit_ack_from_response",
     KeyType::Bool,
     false,
     "acknowledge new orders from the POST orders reply as well as the user channel (default "
     "true)"},
};

std::unique_ptr<Venue> make(VenueId id, const VenueSection& s, const VenueFactoryOptions& opts) {
  coinbase::AdvancedVenueConfig c = coinbase::make_coinbase_advanced_config(s, opts.dry_run);
  c.record_raw_dir = opts.record_raw_dir;
  return std::make_unique<coinbase::CoinbaseAdvancedVenue>(id, std::move(c));
}

}  // namespace

void register_coinbase_advanced_venue(VenueRegistry& r) {
  static_cast<void>(r.add({.name = "coinbase_advanced",
                           .summary = "Coinbase Advanced Trade spot (production, CDP API key)",
                           .keys = kAdvancedKeys,
                           .caps = {.credentials = true,
                                    .order_entry = true,
                                    .replace = false,
                                    .positions = false,
                                    .polls = false,
                                    // GET /orders/historical/fills
                                    .executions = true},
                           .make = &make}));
}

}  // namespace fastmm::venues
