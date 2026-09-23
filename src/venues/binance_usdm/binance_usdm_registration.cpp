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
    {"cancel_on_order_channel_loss",
     KeyType::Bool,
     false,
     "cancel all orders over REST when order entry drops (default true)"},
    {"emit_ack_from_response",
     KeyType::Bool,
     false,
     "acknowledge orders from the request response, not the event stream (default true)"},
    {"depth_limit", KeyType::Int, false, "REST snapshot depth: 5, 10, 20, 50, 100, 500 or 1000"},
    {"key_type", KeyType::String, false, "hmac (default) | ed25519"},
    {"private_key_file",
     KeyType::String,
     false,
     "Ed25519 private key file (PKCS#8 PEM), with key_type = ed25519"},
    {"private_key_env",
     KeyType::String,
     false,
     "environment variable holding the Ed25519 private key PEM (instead of private_key_file)"},
    {"ws_private_url",
     KeyType::String,
     false,
     "private WebSocket URL; empty = derived from ws_url"},
    {"position_from_account_update",
     KeyType::Bool,
     false,
     "correct the engine position from ACCOUNT_UPDATE when it differs from the fills (default "
     "true)"},
};

std::unique_ptr<Venue> make(VenueId id, const VenueSection& s, const VenueFactoryOptions& opts) {
  binance_usdm::BinanceUsdmVenueConfig c = binance_usdm::make_binance_usdm_config(s, opts.dry_run);
  c.record_raw_dir = opts.record_raw_dir;
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
                                    // GET /fapi/v1/userTrades would serve it (7-day window,
                                    // weight 5, `id` per symbol); not written yet.
                                    .executions = false},
                           .make = &make}));
}

}  // namespace fastmm::venues
