// Alpaca US stocks: the registry entry, the keys the connector owns and its factory.
#include "fastmm/venues/alpaca/alpaca_venue.hpp"
#include "fastmm/venues/registry.hpp"

namespace fastmm::venues {

namespace {

constexpr VenueKeySpec kAlpacaKeys[] = {
    {"ws_private_url",
     KeyType::String,
     false,
     "WebSocket URL of trade_updates (order states and executions); empty = "
     "wss://paper-api.alpaca.markets/stream"},
    {"stale_ms",
     KeyType::Int,
     false,
     "no traffic, pongs included, for this long marks the feed stale and pulls the venue's "
     "quotes, ms (default 15000)"},
    {"dead_ms",
     KeyType::Int,
     false,
     "no traffic for this long forces a reconnect, ms (default 30000; pings go every 5 s)"},
    {"requests_per_minute",
     KeyType::Int,
     false,
     "client-side cap on new orders and replaces, per minute; the venue allows 200 requests a "
     "minute (default 200, 80 % of it used)"},
    {"time_in_force",
     KeyType::String,
     false,
     "what a resting order is sent as: day (default) | gtc"},
    {"extended_hours",
     KeyType::Bool,
     false,
     "resting limit orders may trade before and after the regular session (default false)"},
    {"trades", KeyType::Bool, false, "subscribe to trades as well as quotes (default true)"},
    {"allow_offline_reference_data",
     KeyType::Bool,
     false,
     "start without the asset endpoint, using the configured tick and lot (default false)"},
    {"cancel_on_order_channel_loss",
     KeyType::Bool,
     false,
     "cancel the session's open orders over REST when trade_updates drops (default true)"},
};

std::unique_ptr<Venue> make(VenueId id, const VenueSection& s, const VenueFactoryOptions& opts) {
  alpaca::AlpacaVenueConfig c = alpaca::make_alpaca_config(s, opts.dry_run);
  c.record_raw_dir = opts.record_raw_dir;
  c.source = source_address(s);
  return std::make_unique<alpaca::AlpacaVenue>(id, std::move(c));
}

}  // namespace

void register_alpaca_venue(VenueRegistry& r) {
  static_cast<void>(r.add({.name = "alpaca",
                           .summary = "Alpaca US stocks (paper or live account, IEX data)",
                           .keys = kAlpacaKeys,
                           .caps = {.credentials = true,
                                    // the market-data stream authenticates with the keys too
                                    .market_data_keys = true,
                                    .order_entry = true,
                                    .replace = true,
                                    .positions = true,
                                    // GET /v2/positions in every reconciliation
                                    .position_snapshot = true,
                                    .polls = false,
                                    // GET /v2/account/activities/FILL
                                    .executions = true,
                                    .bind_source = true},
                           .make = &make}));
}

}  // namespace fastmm::venues
