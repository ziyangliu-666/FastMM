#pragma once
// SimExchangeServer: the Binance Spot-compatible simulated exchange (plan 8.3, ADR-0008).
//
//   one net::Reactor
//     ├─ HttpServer<PlainStream>            REST /api/v3/* + WebSocket upgrades (default :9080)
//     └─ HttpServer<TlsStream<PlainStream>> the same over TLS (default :9443)
//   WebSocket endpoints: /stream?streams=..., /ws/<stream>, /ws (SUBSCRIBE), /ws/<listenKey>,
//   /ws-api/v3 (WebSocket API incl. userDataStream.subscribe.signature)
//   venue: MatchingEngine (price-time, LIMIT_MAKER rejection) + MarketGenerator counter-party
//   flow + MdAggregator (depthUpdate U/u batches every depth_update_ms, bookTicker, trade)
//
// Deterministic given the seed: generator flow, order ids and trade ids depend only on the
// configuration and on the order of requests; timestamps come from the reactor clock.
//
// Threading: drive it either with poll() on one thread, or start() a background thread. All
// public methods other than poll() are safe to call from any thread while the background
// thread runs (they are marshalled onto the reactor thread and wait for completion).
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/sim/server/sim_server_config.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace fastmm::sim::server {

struct SimServerStats {
  std::int64_t uptime_ms = 0;
  // connections (current / cumulative)
  std::uint64_t http_connections = 0;
  std::uint64_t md_sessions = 0;
  std::uint64_t api_sessions = 0;
  std::uint64_t user_subscriptions = 0;
  std::uint64_t md_sessions_opened = 0;
  std::uint64_t api_sessions_opened = 0;
  std::uint64_t md_connections_dropped = 0;
  std::uint64_t api_connections_dropped = 0;
  // requests
  std::uint64_t rest_requests = 0;
  std::uint64_t ws_api_requests = 0;
  std::uint64_t depth_snapshots = 0;
  std::uint64_t time_requests = 0;
  std::uint64_t open_orders_queries = 0;
  std::uint64_t cancel_all_requests = 0;
  std::uint64_t signature_errors = 0;
  std::uint64_t session_logons = 0;  // successful session.logon (Ed25519 accounts)
  std::uint64_t timestamp_errors = 0;
  std::uint64_t rate_limited = 0;
  std::uint64_t unanswered_rest = 0;
  // trading account
  std::uint64_t orders_accepted = 0;
  std::uint64_t orders_rejected = 0;
  std::uint64_t cancels = 0;
  std::uint64_t cancel_rejects = 0;
  std::uint64_t replaces = 0;
  std::uint64_t amends = 0;
  std::uint64_t fills = 0;
  std::uint64_t open_orders = 0;
  std::uint64_t max_open_orders = 0;
  Qty max_order_qty{};
  Qty position{};          // net filled base quantity on the first symbol
  Qty max_abs_position{};  // largest |position| seen
  Notional fees{};
  Notional cash_flow{};  // quote received minus paid (fees included), first symbol
  Notional pnl{};        // cash_flow + position marked at the first symbol's mid
  // market data
  std::uint64_t generator_actions = 0;
  std::uint64_t trades = 0;
  std::uint64_t depth_updates = 0;
  std::uint64_t depth_updates_skipped = 0;
  std::uint64_t book_tickers = 0;
  std::uint64_t last_update_id = 0;  // first symbol
  Level best_bid{};
  Level best_ask{};
  // since the last mark()
  std::uint64_t min_open_orders_since_mark = 0;
  std::uint64_t orders_since_mark = 0;
  std::uint64_t cancels_since_mark = 0;
  std::uint64_t fills_since_mark = 0;
  std::uint64_t cancel_all_since_mark = 0;
  std::uint64_t open_orders_queries_since_mark = 0;
  std::uint64_t md_sessions_opened_since_mark = 0;
  std::uint64_t api_sessions_opened_since_mark = 0;
};

class SimExchangeServer {
 public:
  explicit SimExchangeServer(SimServerConfig cfg);  // throws std::invalid_argument
  ~SimExchangeServer();
  SimExchangeServer(const SimExchangeServer&) = delete;
  SimExchangeServer& operator=(const SimExchangeServer&) = delete;

  // Binds the plain and TLS listeners and arms the market timers. False + last_error().
  bool listen();
  [[nodiscard]] const std::string& last_error() const noexcept;
  [[nodiscard]] std::uint16_t port() const noexcept;      // 0 when disabled
  [[nodiscard]] std::uint16_t tls_port() const noexcept;  // 0 when disabled
  [[nodiscard]] const SimServerConfig& config() const noexcept;

  // Single-threaded driving: one reactor iteration.
  int poll(int max_wait_ms);
  // Background thread driving.
  void start();
  void stop();
  [[nodiscard]] bool running() const noexcept;

  [[nodiscard]] SimServerStats stats() const;
  [[nodiscard]] std::int64_t server_time_ms() const;
  // Resets the *_since_mark statistics.
  void mark();

  // ---- fault injection (thread-safe) ----------------------------------------------------------
  void drop_market_data_connections();
  // Closes WS API connections that carry no user-data subscription (the order channel); with
  // `include_user_streams` every WS API connection.
  void drop_ws_api_connections(bool include_user_streams = false);
  void skip_next_depth_update();
  void set_ack_delay_ms(std::uint32_t ms);
  void reject_next_orders(std::uint32_t count);
  void fail_next_timestamp();
  void set_rest_unresponsive(bool unresponsive);
  void rate_limit_next_requests(std::uint32_t count);
  void set_clock_offset_ms(std::int64_t offset_ms);
  void expire_listen_keys();

  struct Impl;  // opaque; defined in src/sim/server/server_impl.hpp

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace fastmm::sim::server
