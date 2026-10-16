// Hyperliquid perpetuals: the registry entry, the keys the connector owns and its factory.
#include "fastmm/venues/hyperliquid/hyperliquid_venue.hpp"
#include "fastmm/venues/registry.hpp"

namespace fastmm::venues {

namespace {

constexpr std::string_view kHyperliquidAliases[] = {"hl"};

constexpr VenueKeySpec kHyperliquidKeys[] = {
    {"dex",
     KeyType::String,
     false,
     "builder-deployed perp dex whose coins may be named without their prefix (symbol NVDA "
     "for xyz:NVDA with dex = \"xyz\"); default none"},
    {"vault_address",
     KeyType::String,
     false,
     "trade for this vault or sub-account of the account (0x and 40 hex digits); default none"},
    {"order_api", KeyType::String, false, "order entry: ws (default, WebSocket post) | rest"},
    {"l2_book", KeyType::Bool, false, "subscribe l2Book, 20 levels a side (default true)"},
    {"bbo", KeyType::Bool, false, "subscribe bbo, the touch (default true)"},
    {"trades", KeyType::Bool, false, "subscribe trades (default true)"},
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
     "{\"method\":\"ping\"} interval on every connection, ms, 1000 to 50000 (default 30000; the "
     "venue closes a connection that sent nothing for 60 s)"},
    {"orders_per_second",
     KeyType::Int,
     false,
     "client-side cap on new orders and modifies per second (default 20)"},
    {"posts_per_minute",
     KeyType::Int,
     false,
     "client-side cap on actions sent per minute, at most 2000 (default 1800; the venue allows "
     "2000 WebSocket messages a minute)"},
    {"address_reserve",
     KeyType::Int,
     false,
     "refuse new orders and modifies when no more than this many requests of the address budget "
     "(userRateLimit) would remain (default 200)"},
    {"rate_limit_refresh_s",
     KeyType::Int,
     false,
     "read the address budget (userRateLimit) this often, s, at least 10 (default 60)"},
    {"emit_ack_from_response",
     KeyType::Bool,
     false,
     "acknowledge orders from the action's reply, not the orderUpdates stream (default true)"},
    {"cancel_on_order_channel_loss",
     KeyType::Bool,
     false,
     "cancel all orders over REST when order entry drops (default true)"},
    {"allow_offline_reference_data",
     KeyType::Bool,
     false,
     "start without metaAndAssetCtxs, using the configured tick and lot (default false)"},
    {"dead_mans_switch_s",
     KeyType::Int,
     false,
     "scheduleCancel window in seconds, at least 10, refreshed at a third of it; each refresh "
     "spends one request of the address budget and the venue allows 10 triggers a day. 0 "
     "disables it (default 0)"},
    {"latency_log_s",
     KeyType::Int,
     false,
     "log the post reply round trip and the order open lag (block time - send time) this often, "
     "s; 0 off (default 60)"},
};

std::unique_ptr<Venue> make(VenueId id, const VenueSection& s, const VenueFactoryOptions& opts) {
  hyperliquid::HyperliquidVenueConfig c = hyperliquid::make_hyperliquid_config(s, opts.dry_run);
  c.record_raw_dir = opts.record_raw_dir;
  c.source = source_address(s);
  return std::make_unique<hyperliquid::HyperliquidVenue>(id, std::move(c));
}

}  // namespace

void register_hyperliquid_venue(VenueRegistry& r) {
  static_cast<void>(r.add({.name = "hyperliquid",
                           .summary = "Hyperliquid perpetuals, main and builder-deployed dexes",
                           .aliases = kHyperliquidAliases,
                           .keys = kHyperliquidKeys,
                           .caps = {.credentials = true,
                                    .order_entry = true,
                                    .replace = true,
                                    // clearinghouseState
                                    .positions = true,
                                    .position_snapshot = true,
                                    .polls = false,
                                    // userFillsByTime
                                    .executions = true,
                                    .bind_source = true},
                           .make = &make}));
}

}  // namespace fastmm::venues
