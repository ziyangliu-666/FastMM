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
#include <string_view>
#include <vector>

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
  std::uint64_t my_trades_queries = 0;
  std::uint64_t cancel_all_requests = 0;
  std::uint64_t signature_errors = 0;
  std::uint64_t session_logons = 0;  // successful session.logon (Ed25519 accounts)
  std::uint64_t timestamp_errors = 0;
  std::uint64_t rate_limited = 0;
  std::uint64_t unanswered_rest = 0;
  std::uint64_t key_errors = 0;           // -2015 (including the injected ones)
  std::uint64_t banned_requests = 0;      // 418 answers from ban_next_requests()
  std::uint64_t responses_swallowed = 0;  // WebSocket API replies never sent
  std::uint64_t user_events_dropped = 0;  // dropped while the user stream was muted
  std::uint64_t user_events_duplicated = 0;
  std::uint64_t malformed_frames_sent = 0;
  // trading account
  std::uint64_t orders_accepted = 0;
  std::uint64_t orders_rejected = 0;
  // A clientOrderId this run accepted more than once. The venue refuses a repeat while the first
  // one is open, so this counts ids reused after the first order ended: a restarted engine that
  // forgot its session epoch would show up here.
  std::uint64_t duplicate_client_order_ids = 0;
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
  // Per symbol, in SimServerConfig::symbols order (empty before the first fill): net filled base
  // quantity and fills.
  std::vector<Qty> symbol_positions;
  std::vector<std::uint64_t> symbol_fills;
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
  std::uint64_t my_trades_queries_since_mark = 0;
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

  // Uncertain outcomes. The next `count` WebSocket API requests are carried out in full and their
  // reply is never sent: the venue acted, the client never learns whether it did.
  void swallow_next_ws_api_responses(std::uint32_t count);
  // The next `count` user-data events are delivered twice (a repeated executionReport).
  void duplicate_next_user_events(std::uint32_t count);
  // Drops every user-data event while set, without closing the connection: the private stream is
  // out and, as on a real venue, nothing replays what was missed.
  void set_user_stream_muted(bool muted);
  // Fills up to `qty` of the open order with this client order id by crossing it with a
  // counter-order from the generator account (`qty` zero or larger than the remainder: all of it).
  // Everything ahead of it in price-time is taken first, so the order named is the one that fills.
  // Returns the quantity it actually filled.
  [[nodiscard]] Qty fill_open_order(std::string_view client_order_id, Qty qty = Qty{});
  // Client order ids the account holds open, ascending venue order id.
  [[nodiscard]] std::vector<std::string> open_client_order_ids() const;

  // Venue-side chaos.
  void ban_next_requests(std::uint32_t count);                // 418 + Retry-After, as an IP ban
  void fail_next_auth(std::uint32_t count);                   // 401 / -2015, as a revoked key
  void send_malformed_frames(bool market_data, bool ws_api);  // one non-JSON text frame each

  struct Impl;  // opaque; defined in src/sim/server/server_impl.hpp

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace fastmm::sim::server
