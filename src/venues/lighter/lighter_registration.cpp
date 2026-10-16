// Lighter perpetuals: the registry entry, the keys the connector owns and its factory.
#include "fastmm/venues/lighter/lighter_venue.hpp"
#include "fastmm/venues/registry.hpp"

namespace fastmm::venues {

namespace {

constexpr std::string_view kLighterAliases[] = {"zklighter"};

constexpr VenueKeySpec kLighterKeys[] = {
    {"api_key_index",
     KeyType::Int,
     false,
     "the slot the API key is registered in, 4 to 254 (tools/lighter_api_key.py registers one); "
     "needed with api_secret"},
    {"order_api", KeyType::String, false, "order entry: ws (default, jsonapi/sendtx) | rest"},
    {"order_book", KeyType::Bool, false, "subscribe order_book, the whole book (default true)"},
    {"ticker", KeyType::Bool, false, "subscribe ticker, the touch (default true)"},
    {"trades", KeyType::Bool, false, "subscribe trade (default true)"},
    {"skip_nonce",
     KeyType::Bool,
     false,
     "sign every transaction with the SkipNonce attribute, so one the venue refuses does not "
     "fail the ones after it (default true)"},
    {"stale_ms",
     KeyType::Int,
     false,
     "no traffic for this long marks the feed stale and pulls the venue's quotes, ms (default "
     "5000)"},
    {"dead_ms",
     KeyType::Int,
     false,
     "no traffic for this long forces a reconnect, ms; raised to at least 2 x ping_interval_ms + "
     "5000 (default 60000)"},
    {"ping_interval_ms",
     KeyType::Int,
     false,
     "{\"type\":\"ping\"} interval on every connection, ms, 1000 to 110000 (default 30000; the "
     "venue closes a connection that sent nothing for 2 minutes)"},
    {"orders_per_second",
     KeyType::Int,
     false,
     "client-side cap on new orders and modifies per second (default 20)"},
    {"tx_per_minute",
     KeyType::Int,
     false,
     "client-side cap on transactions sent per minute (default 3600; the venue allows 4000 on a "
     "Premium account with no LIT staked, 60 on a Standard one)"},
    {"quota_reserve",
     KeyType::Int,
     false,
     "refuse new orders and modifies when no more than this much of the volume quota would remain "
     "(default 50)"},
    {"order_expiry_s",
     KeyType::Int,
     false,
     "lifetime of a resting order, s, 300 to 2592000 (default 2419200, 28 days)"},
    {"auth_ttl_s",
     KeyType::Int,
     false,
     "lifetime of the auth token of the account channel and the authenticated REST calls, s, "
     "600 to 28740; the account channel reconnects with a new one at 90 % of it (default 25200)"},
    {"terminal_wait_ms",
     KeyType::Int,
     false,
     "hold an order's cancel or expiry this long for the fills it counts, ms (default 2000)"},
    {"emit_ack_from_response",
     KeyType::Bool,
     false,
     "acknowledge orders from the sendtx reply (accepted) rather than their open on "
     "account_all_orders (default false)"},
    {"cancel_on_order_channel_loss",
     KeyType::Bool,
     false,
     "cancel all orders over REST when order entry drops (default true)"},
    {"allow_offline_reference_data",
     KeyType::Bool,
     false,
     "start without orderBookDetails; no market can be traded then (default false)"},
    {"dead_mans_switch_s",
     KeyType::Int,
     false,
     "scheduled CancelAll window in seconds, 310 to 1296000, refreshed at a third of it over the "
     "trade connection; each refresh is one transaction. 0 disables it (default 0)"},
    {"latency_log_s",
     KeyType::Int,
     false,
     "log the sendtx reply round trip and the order open lag (venue time - send time) this often, "
     "s; 0 off (default 60)"},
};

std::unique_ptr<Venue> make(VenueId id, const VenueSection& s, const VenueFactoryOptions& opts) {
  lighter::LighterVenueConfig c = lighter::make_lighter_config(s, opts.dry_run);
  c.record_raw_dir = opts.record_raw_dir;
  c.source = source_address(s);
  return std::make_unique<lighter::LighterVenue>(id, std::move(c));
}

}  // namespace

void register_lighter_venue(VenueRegistry& r) {
  static_cast<void>(r.add({.name = "lighter",
                           .summary = "Lighter (zkLighter) perpetuals",
                           .aliases = kLighterAliases,
                           .keys = kLighterKeys,
                           .caps = {.credentials = true,
                                    .order_entry = true,
                                    .replace = true,
                                    // account
                                    .positions = true,
                                    .position_snapshot = true,
                                    .polls = false,
                                    .executions = false,
                                    .bind_source = true},
                           .make = &make}));
}

}  // namespace fastmm::venues
