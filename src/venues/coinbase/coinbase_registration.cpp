// Coinbase Exchange spot: the registry entry, the keys the connector owns and its factory.
#include "fastmm/venues/coinbase/coinbase_venue.hpp"
#include "fastmm/venues/registry.hpp"

namespace fastmm::venues {

namespace {

constexpr VenueKeySpec kCoinbaseKeys[] = {
    {"ws_private_url",
     KeyType::String,
     false,
     "WebSocket URL of the user channel (order events); empty = ws_url"},
    {"depth_channel",
     KeyType::String,
     false,
     "order book channel: level2_batch (default; batched every 50 ms, no signature) | level2 "
     "(signed: needs the keys)"},
    {"stp",
     KeyType::String,
     false,
     "self-trade prevention of every order: dc (default) | co | cn | cb"},
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
     "client-side cap on new orders, per second (default 8; the venue allows 15 private REST "
     "requests a second)"},
    {"cancel_all_rounds",
     KeyType::Int,
     false,
     "kill-switch DELETE /orders per product, repeated until one cancels nothing, at most this "
     "often, 1 to 10 (default 3)"},
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
     "acknowledge new orders from the POST /orders reply as well as the user channel (default "
     "true)"},
};

std::unique_ptr<Venue> make(VenueId id, const VenueSection& s, const VenueFactoryOptions& opts) {
  coinbase::CoinbaseVenueConfig c = coinbase::make_coinbase_config(s, opts.dry_run);
  c.record_raw_dir = opts.record_raw_dir;
  c.source = source_address(s);
  return std::make_unique<coinbase::CoinbaseExchangeVenue>(id, std::move(c));
}

}  // namespace

void register_coinbase_venue(VenueRegistry& r) {
  static_cast<void>(r.add({.name = "coinbase_exchange",
                           .summary = "Coinbase Exchange spot (sandbox)",
                           .keys = kCoinbaseKeys,
                           .caps = {.credentials = true,
                                    .order_entry = true,
                                    .replace = false,
                                    .positions = false,
                                    .polls = false,
                                    // GET /fills
                                    .executions = true,
                                    .bind_source = true},
                           .make = &make}));
}

}  // namespace fastmm::venues
