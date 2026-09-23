// Binance Spot: the registry entry, the keys the connector owns and its factory. Everything the
// core needs to know about this venue is here (venues/registry.hpp).
#include "fastmm/venues/binance/binance_venue.hpp"
#include "fastmm/venues/registry.hpp"

namespace fastmm::venues {

namespace {

constexpr std::string_view kBinanceAliases[] = {"binance", "sim"};

constexpr VenueKeySpec kBinanceKeys[] = {
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
    {"cancel_on_order_channel_loss",
     KeyType::Bool,
     false,
     "cancel all orders over REST when order entry drops (default true)"},
    {"emit_ack_from_response",
     KeyType::Bool,
     false,
     "acknowledge orders from the request response, not the event stream (default true)"},
    {"amend_keep_priority",
     KeyType::Bool,
     false,
     "reduce size with order.amend.keepPriority, which keeps the queue position, instead of "
     "order.cancelReplace (default true)"},
    {"max_order_amends",
     KeyType::Int,
     false,
     "keepPriority amendments allowed on one order before falling back to cancelReplace "
     "(default 10, the venue's MAX_NUM_ORDER_AMENDS filter)"},
    {"depth_limit", KeyType::Int, false, "REST snapshot depth, 5 to 5000"},
    {"key_type", KeyType::String, false, "hmac (default) | ed25519"},
    {"private_key_file",
     KeyType::String,
     false,
     "Ed25519 private key file (PKCS#8 PEM), with key_type = ed25519"},
    {"private_key_env",
     KeyType::String,
     false,
     "environment variable holding the Ed25519 private key PEM (instead of private_key_file)"},
    {"md_format",
     KeyType::String,
     false,
     "json (default) | sbe (binary market data; needs an Ed25519 api_key)"},
    {"sbe_ws_url",
     KeyType::String,
     false,
     "SBE stream URL; empty = ws_url with stream. -> stream-sbe."},
    {"user_stream", KeyType::String, false, "ws_api (default) | listen_key | none"},
    {"position_from_balance", KeyType::Bool, false, "derive positions from account balances"},
};

std::unique_ptr<Venue> make(VenueId id, const VenueSection& s, const VenueFactoryOptions& opts) {
  binance::VenueSectionView v;
  v.name = s.name;
  v.ws_url = s.ws_url;
  v.ws_api_url = s.ws_api_url;
  v.rest_url = s.rest_url;
  v.api_key = s.api_key;
  v.api_secret = s.api_secret;
  v.supports_replace = s.supports_replace;
  v.insecure_tls = s.insecure_tls;
  v.ca_file = s.ca_file;
  v.recv_window_ms = s.recv_window_ms;
  v.extra = &s.extra;
  binance::BinanceVenueConfig c = binance::make_binance_config(v, opts.dry_run);
  c.record_raw_dir = opts.record_raw_dir;
  return std::make_unique<binance::BinanceVenue>(id, std::move(c));
}

}  // namespace

void register_binance_venue(VenueRegistry& r) {
  static_cast<void>(r.add({.name = "binance_spot",
                           .summary = "Binance Spot (testnet, Demo Mode, or the Binance-compatible "
                                      "simulator with kind = \"sim\")",
                           .aliases = kBinanceAliases,
                           .keys = kBinanceKeys,
                           .caps = {.credentials = true,
                                    .order_entry = true,
                                    .replace = true,
                                    .positions = true,
                                    .polls = false,
                                    .executions = true},
                           .make = &make}));
}

}  // namespace fastmm::venues
