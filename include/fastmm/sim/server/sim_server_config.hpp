#pragma once
// Configuration of the Binance-compatible simulated exchange (fastmm-sim-exchange, 8.3). Read
// from the [[instruments]] and [sim] sections of a FastMM TOML file (configs/sim.toml), or
// built in code by tests (SimServerConfig::defaults()).
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/sim/market_generator.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace fastmm {
class Config;
}

namespace fastmm::sim::server {

struct SimSymbolConfig {
  std::string symbol;       // "BTCUSDT"
  std::string base_asset;   // "BTC"
  std::string quote_asset;  // "USDT"
  Price tick{};
  Qty lot{};
  Qty min_qty{};
  Qty max_qty{};  // 0 = no maximum
  Notional min_notional{};
  Notional max_notional{};  // 0 = no maximum
  Price start_mid{};
};

struct SimBalanceConfig {
  std::string asset;
  Qty amount{};
};

// [sim.faults]. Every *_after_ms entry is a one-shot relative to listen(); 0 disables it.
struct FaultConfig {
  std::uint32_t drop_md_after_ms = 0;            // close every market-data WebSocket once
  std::uint32_t drop_ws_api_after_ms = 0;        // close order-entry WS API connections once
  std::uint32_t skip_depth_update_after_ms = 0;  // omit one depthUpdate (connector must resync)
  std::uint32_t timestamp_error_after_ms = 0;    // next signed request answers -1021
  std::uint32_t rest_unresponsive_after_ms = 0;  // REST requests are swallowed from then on
  std::uint32_t rest_unresponsive_for_ms = 0;    // ... for this long (0 = until cleared)
  std::uint32_t delay_ack_ms = 0;                // WS API order responses + their user events
  std::uint32_t reject_next_orders = 0;          // next K new orders answer -2010
  std::uint32_t rate_limit_next = 0;             // next K REST requests answer 429 / -1003
};

struct SimServerConfig {
  // network
  std::string bind_host = "127.0.0.1";
  int port = 9080;      // 0 = ephemeral, < 0 = disabled
  int tls_port = 9443;  // 0 = ephemeral, < 0 = disabled
  std::string tls_cert = "tests/fixtures/tls/cert.pem";
  std::string tls_key = "tests/fixtures/tls/key.pem";
  // [engine] net_backend; io_uring falls back to epoll (with a warning) when unsupported.
  net::ReactorBackend net_backend = net::ReactorBackend::Epoll;

  // the single trading account; the secret is never logged. With an Ed25519 public key (PEM,
  // "-----BEGIN PUBLIC KEY-----") the account key is an Ed25519 key: signatures are verified
  // with it, session.logon works and api_secret is not used.
  std::string api_key = "sim-key";
  std::string api_secret = "sim-secret";
  std::string ed25519_public_key_pem;
  std::vector<SimBalanceConfig> balances;  // empty: 100 base + 10,000,000 quote per symbol

  // market
  std::uint64_t seed = 7;
  std::vector<SimSymbolConfig> symbols;
  MarketGeneratorParams generator;  // start_mid / tick / lot are taken per symbol
  bool generator_enabled = true;
  int seed_levels = 20;
  double maker_bps = 1.0;
  double taker_bps = 4.0;
  std::uint32_t depth_update_ms = 100;  // depthUpdate aggregation interval (@depth@100ms)
  // GET /api/v3/depth: false = the live book (Binance; lastUpdateId usually falls inside the
  // next depthUpdate batch), true = the book as of the last published batch (lastUpdateId is a
  // batch boundary, so the next event starts at lastUpdateId + 1). Both are valid inputs for
  // the documented sync algorithm; "flushed" makes tests independent of that race.
  bool snapshot_at_flush = false;
  std::uint32_t driver_tick_ms = 2;  // generator / aggregator poll period

  // clock: published timestamps and recvWindow checks use the host wall clock + offset. With
  // start_time_ms > 0 they use start_time + reactor-clock elapsed instead (reproducible, but
  // clients then depend on their measured clock offset staying valid).
  std::int64_t start_time_ms = 0;
  std::int64_t clock_offset_ms = 0;

  // limits (exchangeInfo.rateLimits)
  std::uint32_t weight_limit_per_minute = 6000;
  std::uint32_t orders_limit_per_10s = 1000;
  std::uint32_t orders_limit_per_day = 1'000'000;
  std::uint32_t max_recv_window_ms = 60'000;
  std::uint32_t max_open_orders_per_symbol = 200;

  // WebSocket housekeeping
  std::uint32_t ping_interval_ms = 20'000;
  std::uint32_t pong_timeout_ms = 60'000;
  std::uint32_t listen_key_validity_ms = 3'600'000;

  FaultConfig faults;

  // Reads [[instruments]] and [sim] (see configs/sim.toml). FASTMM_SIM_API_KEY /
  // FASTMM_SIM_API_SECRET in the environment override the configured key. Throws
  // std::invalid_argument on bad values.
  static SimServerConfig from_config(const Config& cfg);
  static SimServerConfig load(const std::string& path);
  // BTCUSDT with configs/sim-local.toml's filters and a generator tuned so that BasicMM's
  // 5 bps quotes get filled.
  static SimServerConfig defaults();
};

}  // namespace fastmm::sim::server
