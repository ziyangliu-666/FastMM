// Deribit options and futures: the registry entry, the keys the connector owns and its factory.
#include "fastmm/venues/deribit/deribit_venue.hpp"
#include "fastmm/venues/registry.hpp"

namespace fastmm::venues {

namespace {

constexpr VenueKeySpec kDeribitKeys[] = {
    {"stale_ms",
     KeyType::Int,
     false,
     "no traffic for this long marks the feed stale and pulls the venue's quotes, ms (default "
     "10000)"},
    {"dead_ms",
     KeyType::Int,
     false,
     "no traffic for this long forces a reconnect, ms; raised to three heartbeat intervals, "
     "30000 by default"},
    {"allow_offline_reference_data",
     KeyType::Bool,
     false,
     "start without REST reference data, using the configured tick and lot (default false)"},
    {"cancel_on_order_channel_loss",
     KeyType::Bool,
     false,
     "cancel all orders over REST when order entry drops (default true)"},
    {"emit_ack_from_response",
     KeyType::Bool,
     false,
     "acknowledge orders from the request response, not the event stream (default true)"},
    {"ws_private_url",
     KeyType::String,
     false,
     "private WebSocket URL; empty = derived from ws_url"},
    {"currencies",
     KeyType::Any,
     false,
     "currencies for reference data, user channels and reconciliation, \"BTC\" or [\"BTC\", "
     "\"ETH\"] (default \"BTC\")"},
    {"book_interval", KeyType::String, false, "book channel interval, 100ms (default) | agg2"},
    {"ticker_interval", KeyType::String, false, "ticker channel interval, 100ms (default) | agg2"},
    {"trades_interval", KeyType::String, false, "trades channel interval, 100ms (default) | agg2"},
    {"heartbeat_interval_s", KeyType::Int, false, "public/set_heartbeat interval, s, at least 10"},
    {"reject_post_only",
     KeyType::Bool,
     false,
     "reject crossing post-only orders instead of repricing them (default true)"},
    {"cancel_on_disconnect",
     KeyType::Bool,
     false,
     "cancel-on-disconnect on the order connection (default true)"},
    {"matching_engine_rate",
     KeyType::Int,
     false,
     "order requests per second of the account tier (default 5)"},
    {"matching_engine_burst",
     KeyType::Int,
     false,
     "order request burst of the account tier (default 20)"},
};

std::unique_ptr<Venue> make(VenueId id, const VenueSection& s, const VenueFactoryOptions& opts) {
  deribit::DeribitVenueConfig c = deribit::make_deribit_config(s, opts.dry_run);
  c.record_raw_dir = opts.record_raw_dir;
  return std::make_unique<deribit::DeribitVenue>(id, std::move(c));
}

}  // namespace

void register_deribit_venue(VenueRegistry& r) {
  static_cast<void>(r.add({.name = "deribit",
                           .summary = "Deribit options and futures (testnet)",
                           .keys = kDeribitKeys,
                           .caps = {.credentials = true,
                                    .order_entry = true,
                                    .replace = true,
                                    .positions = true,
                                    .polls = false,
                                    // private/get_user_trades_by_instrument would serve it
                                    // (`trade_id`, `historical` splits the last 24 h from the
                                    // rest); not written yet.
                                    .executions = false},
                           .make = &make}));
}

}  // namespace fastmm::venues
