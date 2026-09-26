// OKX v5 USDT-margined perpetual swaps: the registry entry, the keys the connector owns and its
// factory.
#include "fastmm/venues/okx/okx_venue.hpp"
#include "fastmm/venues/registry.hpp"

namespace fastmm::venues {

namespace {

constexpr VenueKeySpec kOkxKeys[] = {
    {"ws_private_url",
     KeyType::String,
     false,
     "private WebSocket URL (login, orders, positions); empty = ws_url's host + /ws/v5/private"},
    {"td_mode", KeyType::String, false, "margin mode of every order: cross (default) | isolated"},
    {"depth_channel",
     KeyType::String,
     false,
     "order book channel: books (default; 400 levels, 100 ms) | books50-l2-tbt | books-l2-tbt "
     "(10 ms; VIP4 and a login)"},
    {"order_api", KeyType::String, false, "order entry: ws (default) | rest"},
    {"dead_mans_switch_s",
     KeyType::Int,
     false,
     "cancel-all-after countdown, seconds, 10 to 120, refreshed every third of it; the venue "
     "cancels every pending order of the account when it runs out. 0 disables it (default 60)"},
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
     "\"ping\" interval on every connection, ms, 1000 to 25000 (default 20000; OKX closes a "
     "connection idle for 30 s)"},
    {"orders_per_second",
     KeyType::Int,
     false,
     "client-side cap on new orders and amends, per second (default 25; OKX allows 60 per 2 s per "
     "instrument)"},
    {"position_from_stream",
     KeyType::Bool,
     false,
     "correct the engine position from the positions channel when it differs from the fills "
     "(default true)"},
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
     "acknowledge new orders from the request response, not the orders channel (default true)"},
};

std::unique_ptr<Venue> make(VenueId id, const VenueSection& s, const VenueFactoryOptions& opts) {
  okx::OkxVenueConfig c = okx::make_okx_config(s, opts.dry_run);
  c.record_raw_dir = opts.record_raw_dir;
  return std::make_unique<okx::OkxVenue>(id, std::move(c));
}

}  // namespace

void register_okx_venue(VenueRegistry& r) {
  static_cast<void>(r.add({.name = "okx",
                           .summary = "OKX v5 USDT-margined perpetual swaps (demo trading)",
                           .keys = kOkxKeys,
                           .caps = {.credentials = true,
                                    .order_entry = true,
                                    .replace = true,
                                    // account/positions and the positions channel
                                    .positions = true,
                                    .polls = false,
                                    // GET /api/v5/trade/fills
                                    .executions = true},
                           .make = &make}));
}

}  // namespace fastmm::venues
