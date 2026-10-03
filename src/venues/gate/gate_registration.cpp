// Gate USDT perpetual futures: the registry entry, the keys the connector owns and its factory.
#include "fastmm/venues/gate/gate_usdt_venue.hpp"
#include "fastmm/venues/registry.hpp"

namespace fastmm::venues {

namespace {

constexpr std::string_view kGateAliases[] = {"gate", "gate_futures"};

constexpr VenueKeySpec kGateKeys[] = {
    {"settle",
     KeyType::String,
     false,
     "settlement currency of the futures: usdt (default) | usd1 | btc; ws_url and rest_url must "
     "match it when set"},
    {"order_api", KeyType::String, false, "order entry: ws (default, futures.order_place) | rest"},
    {"book_level",
     KeyType::Int,
     false,
     "futures.obu depth: 50 (pushed every 20 ms, default) | 400 (every 100 ms)"},
    {"stale_ms",
     KeyType::Int,
     false,
     "no traffic for this long marks the feed stale and pulls the venue's quotes, ms (default "
     "2000)"},
    {"dead_ms",
     KeyType::Int,
     false,
     "no traffic for this long forces a reconnect, ms; raised to at least 2 x ping_interval_ms + "
     "5000"},
    {"ping_interval_ms",
     KeyType::Int,
     false,
     "futures.ping interval on every connection, ms, at least 1000 (default 15000)"},
    {"orders_per_second",
     KeyType::Int,
     false,
     "client-side cap on new orders and amends per second (default 50; the venue allows 100 per "
     "UID, 200 cancels)"},
    {"emit_ack_from_response",
     KeyType::Bool,
     false,
     "acknowledge orders from the order_place reply, not the orders channel (default true)"},
    {"position_from_stream",
     KeyType::Bool,
     false,
     "correct the engine position from the positions channel when it differs from the fills "
     "(default true)"},
    {"cancel_on_order_channel_loss",
     KeyType::Bool,
     false,
     "cancel all orders over REST when order entry drops (default true)"},
    {"allow_offline_reference_data",
     KeyType::Bool,
     false,
     "start without the contract table, using the configured tick, lot and multiplier (default "
     "false)"},
    {"fetch_fees",
     KeyType::Bool,
     false,
     "read the account's maker/taker rates from GET /futures/{settle}/fee at start (default "
     "false)"},
    {"dead_mans_switch_s",
     KeyType::Int,
     false,
     "POST /futures/{settle}/countdown_cancel_all window in seconds, refreshed at a third of it; "
     "the venue cancels every order of the account when no refresh arrives. 0 disables it "
     "(default 0)"},
};

std::unique_ptr<Venue> make(VenueId id, const VenueSection& s, const VenueFactoryOptions& opts) {
  gate::GateUsdtVenueConfig c = gate::make_gate_usdt_config(s, opts.dry_run);
  c.record_raw_dir = opts.record_raw_dir;
  return std::make_unique<gate::GateUsdtVenue>(id, std::move(c));
}

}  // namespace

void register_gate_usdt_venue(VenueRegistry& r) {
  static_cast<void>(r.add({.name = "gate_usdt",
                           .summary = "Gate APIv4 USDT-settled perpetual futures",
                           .aliases = kGateAliases,
                           .keys = kGateKeys,
                           .caps = {.credentials = true,
                                    .order_entry = true,
                                    .replace = true,
                                    // GET /positions and the positions channel
                                    .positions = true,
                                    .polls = false,
                                    // GET /my_trades_timerange
                                    .executions = true},
                           .make = &make}));
}

}  // namespace fastmm::venues
