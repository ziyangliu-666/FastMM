// Bybit v5 spot and linear perpetuals: the registry entry, the keys the connector owns and its
// factory.
#include "fastmm/venues/bybit/bybit_venue.hpp"
#include "fastmm/venues/registry.hpp"

namespace fastmm::venues {

namespace {

constexpr std::string_view kBybitAliases[] = {"bybit_spot"};

constexpr VenueKeySpec kBybitKeys[] = {
    {"category",
     KeyType::String,
     false,
     "product: spot (default) | linear (USDT- and USDC-margined perpetuals, one-way position mode "
     "only); ws_url must be the matching public stream"},
    {"stale_ms",
     KeyType::Int,
     false,
     "no traffic for this long marks the feed stale and pulls the venue's quotes, ms (default "
     "2000)"},
    {"dead_ms",
     KeyType::Int,
     false,
     "no traffic for this long forces a reconnect, ms; raised to at least 45000"},
    {"order_api", KeyType::String, false, "order entry: ws (default) | rest"},
    {"allow_offline_reference_data",
     KeyType::Bool,
     false,
     "start without REST reference data, using the configured tick and lot (default false)"},
    {"dead_mans_switch_s",
     KeyType::Int,
     false,
     "Bybit disconnect-cancel-all window in seconds, 3 to 300; the venue cancels every order of "
     "the category (product SPOT, or DERIVATIVES for linear) once no private connection is left. "
     "0 disables it (default 0: Bybit only grants DCP to institutional accounts)"},
    {"cancel_on_order_channel_loss",
     KeyType::Bool,
     false,
     "cancel all orders over REST when order entry drops (default true)"},
    {"emit_ack_from_response",
     KeyType::Bool,
     false,
     "acknowledge orders from the request response, not the event stream (default true)"},
    {"depth", KeyType::Int, false, "order book subscription depth, 1 to 1000"},
    {"ws_private_url",
     KeyType::String,
     false,
     "private WebSocket URL; empty = derived from ws_url"},
    {"ping_interval_ms", KeyType::Int, false, "application ping interval, ms, at least 1000"},
    {"orders_per_second", KeyType::Int, false, "client-side order rate cap, orders/s"},
    {"position_from_wallet", KeyType::Bool, false, "spot: derive positions from the wallet"},
    {"position_from_stream",
     KeyType::Bool,
     false,
     "linear: correct the engine position from the position topic when it differs from the fills "
     "(default true)"},
};

std::unique_ptr<Venue> make(VenueId id, const VenueSection& s, const VenueFactoryOptions& opts) {
  bybit::BybitVenueConfig c = bybit::make_bybit_config(s, opts.dry_run);
  c.record_raw_dir = opts.record_raw_dir;
  return std::make_unique<bybit::BybitVenue>(id, std::move(c));
}

}  // namespace

void register_bybit_venue(VenueRegistry& r) {
  static_cast<void>(r.add({.name = "bybit",
                           .summary = "Bybit v5 spot and linear perpetuals (testnet)",
                           .aliases = kBybitAliases,
                           .keys = kBybitKeys,
                           .caps = {.credentials = true,
                                    .order_entry = true,
                                    .replace = true,
                                    // spot: the wallet; linear: position/list and the
                                    // position topic
                                    .positions = true,
                                    .polls = false,
                                    // GET /v5/execution/list
                                    .executions = true},
                           .make = &make}));
}

}  // namespace fastmm::venues
