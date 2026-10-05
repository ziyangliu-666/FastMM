// Binance USDⓈ-M: the registry entry, the keys the connector owns and its factory.
#include "fastmm/venues/binance_usdm/binance_usdm_venue.hpp"
#include "fastmm/venues/registry.hpp"

namespace fastmm::venues {

namespace {

constexpr VenueKeySpec kBinanceUsdmKeys[] = {
    {"stale_ms",
     KeyType::Int,
     false,
     "no traffic for this long marks the feed stale and pulls the venue's quotes, ms (default "
     "2000)"},
    {"dead_ms",
     KeyType::Int,
     false,
     "no traffic for this long forces a reconnect, ms; raised to at least 45000 for market data "
     "and 240000 for the other channels"},
    {"order_api", KeyType::String, false, "order entry: ws (default) | rest"},
    {"allow_offline_reference_data",
     KeyType::Bool,
     false,
     "start without REST reference data, using the configured tick and lot (default false)"},
    {"dead_mans_switch_ms",
     KeyType::Int,
     false,
     "venue-side countdownCancelAll window in ms; the venue cancels every open order of a "
     "symbol if the connector goes quiet for this long. 0 disables it (default 60000)"},
    {"post_only_rpi",
     KeyType::Bool,
     false,
     "send post-only orders as RPI (matched only against app and web orders, hidden from the "
     "API depth) instead of GTX; needs supports_replace = false (default false)"},
    {"cancel_on_order_channel_loss",
     KeyType::Bool,
     false,
     "cancel all orders over REST when order entry drops (default true)"},
    {"emit_ack_from_response",
     KeyType::Bool,
     false,
     "acknowledge orders from the request response, not the event stream (default true)"},
    {"depth_limit",
     KeyType::Int,
     false,
     "REST snapshot depth: 5, 10, 20, 50, 100, 500 or 1000 (weight 2, 5, 10 or 20); default: 1000 "
     "with up to 10 subscribed symbols, else 100"},
    {"key_type", KeyType::String, false, "hmac (default) | ed25519"},
    {"private_key_file",
     KeyType::String,
     false,
     "Ed25519 private key file (PKCS#8 PEM), with key_type = ed25519"},
    {"private_key_env",
     KeyType::String,
     false,
     "environment variable holding the Ed25519 private key PEM (instead of private_key_file)"},
    {"md_ticker_conns",
     KeyType::Int,
     false,
     "extra connections carrying only the bookTicker streams; the first copy of each update is "
     "used, which takes the occasional slow push server out of the path (0 to 8, default 0)"},
    {"ws_private_url",
     KeyType::String,
     false,
     "private WebSocket URL; empty = derived from ws_url"},
    {"position_from_account_update",
     KeyType::Bool,
     false,
     "correct the engine position from ACCOUNT_UPDATE when it differs from the fills (default "
     "true)"},
    {"one_way_mode",
     KeyType::Bool,
     false,
     "at start, switch an account in hedge mode to one-way mode when it holds no position and no "
     "open order (default false: a hedge-mode account refuses to start)"},
    {"leverage",
     KeyType::Int,
     false,
     "at start, set this leverage (1 to 125) on every enabled symbol; 0 leaves the account's "
     "(default 0)"},
};

std::unique_ptr<Venue> make(VenueId id, const VenueSection& s, const VenueFactoryOptions& opts) {
  binance_usdm::BinanceUsdmVenueConfig c = binance_usdm::make_binance_usdm_config(s, opts.dry_run);
  c.record_raw_dir = opts.record_raw_dir;
  c.pool_of = opts.pool_of;
  c.share_ip_weight = opts.pooled;
  return std::make_unique<binance_usdm::BinanceUsdmVenue>(id, std::move(c));
}

}  // namespace

void register_binance_usdm_venue(VenueRegistry& r) {
  static_cast<void>(r.add({.name = "binance_usdm",
                           .summary = "Binance USDⓈ-M perpetual futures (Demo Trading)",
                           .keys = kBinanceUsdmKeys,
                           .caps = {.credentials = true,
                                    .order_entry = true,
                                    .replace = true,
                                    .positions = true,
                                    .polls = false,
                                    // GET /fapi/v1/userTrades
                                    .executions = true,
                                    .account_pools = true},
                           .make = &make}));
}

}  // namespace fastmm::venues
