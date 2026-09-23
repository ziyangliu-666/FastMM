#pragma once
// Private implementation of SimExchangeServer (one translation-unit family: lifecycle and
// sessions in sim_exchange_server.cpp, trading in order_handlers.cpp, request routing in
// api_routes.cpp). Everything here runs on the reactor thread.
#include "fastmm/net/http_server.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/tls_stream.hpp"
#include "fastmm/net/ws_server.hpp"
#include "fastmm/sim/fee_model.hpp"
#include "fastmm/sim/market_generator.hpp"
#include "fastmm/sim/matching_engine.hpp"
#include "fastmm/sim/md_aggregator.hpp"
#include "fastmm/sim/server/binance_json.hpp"
#include "fastmm/sim/server/request.hpp"
#include "fastmm/sim/server/sim_exchange_server.hpp"
#include "fastmm/sim/server/venue_state.hpp"

#include <atomic>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fastmm::sim::server {

using TlsServerStream = net::TlsStream<net::PlainStream>;

inline constexpr std::int64_t kNsPerMs = 1'000'000;

enum class StreamKind : std::uint8_t { Depth = 0, Ticker = 1, Trade = 2 };

struct StreamSub {
  std::uint32_t symbol = 0;
  StreamKind kind = StreamKind::Depth;
  std::string name;  // as subscribed, echoed in the combined-stream wrapper
};

enum class SessionKind : std::uint8_t { MarketData = 0, WsApi = 1, ListenKey = 2 };

struct SessionState {
  struct UserSub {
    std::int64_t id = 0;
    AccountId account = kStrategyAccount;
  };
  SessionKind kind = SessionKind::MarketData;
  std::uint64_t token = 0;  // distinguishes a reused WsSession address
  bool combined = false;    // /stream wrapper {"stream","data"}
  std::vector<StreamSub> streams;
  std::vector<UserSub> user_subs;
  std::int64_t next_subscription_id = 0;
  std::string listen_key;
  AccountId listen_account = kStrategyAccount;
  // session.logon (Ed25519 accounts): requests without apiKey/signature act as this account.
  Account* logon_account = nullptr;
  std::int64_t authorized_since_ms = 0;
  std::int64_t connected_since_ms = 0;
  std::int64_t last_rx_ns = 0;
};

struct ListenKey {
  AccountId account = kStrategyAccount;
  std::int64_t expires_ms = 0;
};

// Result of one API operation; REST and the WebSocket API wrap it differently.
struct OpResult {
  int status = 200;
  bool is_error = false;
  std::int64_t retry_after_s = 0;  // 429/418: Retry-After header
  std::string body;                // result JSON or the error object {"code","msg"[,"data"]}

  static OpResult ok(std::string body) {
    OpResult r;
    r.body = std::move(body);
    return r;
  }
  static OpResult error(int status, int code, std::string_view msg) {
    OpResult r;
    r.status = status;
    r.is_error = true;
    append_error(r.body, code, msg);
    return r;
  }
  static OpResult error_data(int status, int code, std::string_view msg, std::string_view data) {
    OpResult r;
    r.status = status;
    r.is_error = true;
    append_error_with_data(r.body, code, msg, data);
    return r;
  }
};

enum class RestEndpoint : std::uint8_t {
  Ping,
  Time,
  ExchangeInfo,
  Depth,
  BookTicker,
  AccountInfo,
  NewOrder,
  TestOrder,
  CancelOrder,
  QueryOrder,
  CancelReplace,
  Amend,
  OpenOrders,
  CancelAll,
  ListenKeyCreate,
  ListenKeyKeepalive,
  ListenKeyClose,
};

enum class RespType : std::uint8_t { Ack, Result, Full };

struct NewOrderSpec {
  std::uint32_t symbol = 0;
  Side side = Side::Buy;
  BinanceOrderType type = BinanceOrderType::Limit;
  TimeInForce tif = TimeInForce::Gtc;
  Price price{};
  Qty qty{};
  std::string client_order_id;
  RespType resp = RespType::Ack;
};

struct CancelOutcome {
  bool ok = false;
  int status = 400;
  int code = -2011;
  std::string msg = "Unknown order sent.";
  std::string result;  // cancel result JSON when ok
};

struct CapturedFill {
  Price price{};
  Qty qty{};
  Notional commission{};
  std::uint64_t trade_id = 0;
};

struct FaultState {
  std::uint32_t delay_ack_ms = 0;
  std::uint32_t reject_next = 0;
  std::uint32_t rate_limit_next = 0;
  std::uint32_t swallow_ws_api_next = 0;
  std::uint32_t duplicate_user_events_next = 0;
  std::uint32_t ban_next = 0;
  std::uint32_t auth_fail_next = 0;
  bool user_stream_muted = false;
  bool timestamp_once = false;
  bool rest_unresponsive = false;
  bool skip_depth = false;
  // one-shot schedule bookkeeping
  bool drop_md_done = false;
  bool drop_api_done = false;
  bool skip_depth_done = false;
  bool timestamp_done = false;
  bool rest_start_done = false;
  bool rest_end_done = false;
};

struct SymbolRuntime {
  SimSymbolConfig cfg;
  std::string lower;  // "btcusdt"
  // Book as of the last published depthUpdate batch (snapshot_at_flush).
  std::vector<Level> flushed_bids;
  std::vector<Level> flushed_asks;
  std::size_t flushed_nb = 0;
  std::size_t flushed_na = 0;
  std::uint64_t flushed_update_id = 0;
};

struct SimExchangeServer::Impl final : public net::WsSessionHandler, public MatchingSink {
  explicit Impl(SimServerConfig c);
  ~Impl() override;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  // ---- lifecycle (sim_exchange_server.cpp) -----------------------------------------------
  bool listen();
  void start_thread();
  void stop_thread();
  template <class F>
  auto call(F&& f) -> decltype(f()) {
    if (!running_.load(std::memory_order_acquire) || std::this_thread::get_id() == thread_id_)
      return f();
    std::packaged_task<decltype(f())()> task(std::forward<F>(f));
    auto fut = task.get_future();
    reactor_.post([&task] { task(); });
    return fut.get();
  }
  [[nodiscard]] Timestamp sim_now() const noexcept {
    return Timestamp{start_epoch_ns_ + (net::Reactor::now_ns() - start_mono_ns_)};
  }
  // Venue time in every timestamp the server publishes and in recvWindow checks. It follows
  // the host wall clock (like a real venue, and robust against wall-clock steps, which WSL2
  // makes regularly); with start_time_ms configured it is the reproducible matching clock.
  [[nodiscard]] std::int64_t server_ms() const noexcept {
    const std::int64_t base =
        cfg_.start_time_ms > 0 ? sim_now().ns / kNsPerMs : wall_now().ns / kNsPerMs;
    return base + clock_offset_ms_;
  }
  [[nodiscard]] std::int64_t uptime_ms() const noexcept {
    return listen_mono_ns_ == 0 ? 0 : (net::Reactor::now_ns() - listen_mono_ns_) / kNsPerMs;
  }
  [[nodiscard]] SimServerStats snapshot_stats();
  void mark();

  // timers
  void on_driver();
  void on_housekeeping();
  void on_ping_timer();
  void run_scheduled_faults();

  // sessions / WebSocket handler
  bool accept_upgrade(std::string_view path, std::string_view query) override;
  void on_open(net::WsSession& s) override;
  void on_text(net::WsSession& s, std::string_view text) override;
  void on_pong(net::WsSession& s, std::span<const std::byte>) override;
  void on_close(net::WsSession& s, std::uint16_t, std::string_view) override;
  void on_error(net::WsSession& s, net::NetError, std::string_view) override;
  [[nodiscard]] SessionState* session_state(net::WsSession* s) noexcept;
  bool parse_stream_name(std::string_view name, StreamSub& out) const;
  void handle_stream_control(net::WsSession& s, std::string_view text);
  void deliver(net::WsSession* s, std::uint64_t token, std::string_view text);
  void send_to_session(net::WsSession* s,
                       std::uint64_t token,
                       std::string text,
                       std::uint32_t delay_ms);
  void close_session(net::WsSession* s);

  // faults
  void drop_market_data();
  void drop_ws_api(bool include_user_streams);
  void expire_all_listen_keys();
  void send_malformed(bool market_data, bool ws_api);
  [[nodiscard]] Qty force_fill(std::string_view client_order_id, Qty qty);
  [[nodiscard]] std::vector<std::string> open_client_ids();
  // 418 / -2015 injection shared by REST and the WebSocket API; nullopt = serve the request.
  [[nodiscard]] std::optional<OpResult> injected_stop(std::int64_t now_ms);

  // market data
  static void md_trampoline(void* ctx, EventHeader& h, Timestamp ts) noexcept;
  void on_md_event(EventHeader& h);
  void publish_md(std::uint32_t symbol, StreamKind kind, std::string_view data);
  void capture_flushed_book(std::uint32_t symbol);

  // ---- MatchingSink (order_handlers.cpp) ---------------------------------------------------
  void on_ack(const SimOrder& o, Timestamp) override;
  void on_reject(const NewOrder& n, RejectReason why, Timestamp) override;
  void on_cancel(const SimOrder& o, CancelReason why, Timestamp) override;
  void on_fill(const SimOrder& maker, const SimOrder& taker, Price px, Qty qty, Timestamp) override;
  void on_book_change(InstrumentId id, Side side, Price px, Qty qty, std::uint64_t uid) override;
  void on_trade(InstrumentId id,
                Price px,
                Qty qty,
                Side aggressor,
                std::uint64_t trade_id,
                Timestamp ts) override;

  // ---- trading (order_handlers.cpp) -------------------------------------------------------
  void begin_request(std::uint32_t event_delay_ms);
  void end_request();
  void emit_user_event(AccountId account, std::string event_json);
  void publish_user_event(AccountId account, std::string_view event_json, std::uint32_t delay_ms);
  void queue_balance_updates();
  void refresh_order_stats();
  [[nodiscard]] Account* find_account(std::string_view api_key) noexcept;
  [[nodiscard]] Account& account(AccountId id) noexcept;
  [[nodiscard]] std::optional<std::uint32_t> find_symbol(std::string_view symbol) const noexcept;
  [[nodiscard]] OrderView view_of(const OrderRecord& r) const;
  void emit_exec(const OrderRecord& r,
                 ExecType x,
                 std::string_view client_id_override,
                 std::string_view orig_client_id,
                 const CapturedFill* fill = nullptr,
                 bool maker = false);
  void apply_fill(const SimOrder& o, Price px, Qty qty, bool maker, std::uint64_t trade_id);
  void release_lock(OrderRecord& r);
  [[nodiscard]] std::string next_client_id(std::string_view prefix);

  // timestamp / recvWindow checks shared by signed and session-authenticated requests.
  std::optional<OpResult> check_timing(const ParamList& params, std::int64_t now_ms);
  OpResult op_session(net::WsSession& s,
                      std::string_view method,
                      const ParamList& p,
                      std::int64_t now_ms);
  std::optional<OpResult> authenticate(std::string_view api_key,
                                       const ParamList& params,
                                       std::string_view payload,
                                       std::string_view signature,
                                       std::int64_t now_ms,
                                       Account*& out);
  std::optional<OpResult> parse_new_order(const ParamList& p, NewOrderSpec& out);
  OpResult submit_new_order(Account& a, const NewOrderSpec& spec, bool test_only);
  CancelOutcome cancel_order(Account& a,
                             std::uint32_t symbol,
                             std::string_view order_id,
                             std::string_view orig_client_id,
                             std::string_view new_client_id);
  OpResult op_place(Account& a, const ParamList& p, bool test_only);
  OpResult op_cancel(Account& a, const ParamList& p);
  OpResult op_cancel_replace(Account& a, const ParamList& p);
  OpResult op_amend(Account& a, const ParamList& p);
  OpResult op_query_order(Account& a, const ParamList& p);
  OpResult op_open_orders(Account& a, const ParamList& p);
  OpResult op_cancel_all(Account& a, const ParamList& p);
  OpResult op_account(Account& a);
  OpResult op_exchange_info(const ParamList& p);
  OpResult op_depth(const ParamList& p);
  OpResult op_book_ticker(const ParamList& p);
  OpResult op_listen_key(RestEndpoint ep, const net::HttpRequest& req, const ParamList& p);

  // ---- routing (api_routes.cpp) -----------------------------------------------------------
  template <class Stream>
  void install_routes(net::HttpServer<Stream>& srv);
  net::HttpServerResponse handle_rest(const net::HttpRequest& req, RestEndpoint ep);
  OpResult dispatch_rest(const net::HttpRequest& req,
                         RestEndpoint ep,
                         const ParamList& params,
                         std::int64_t now_ms);
  void handle_ws_api(net::WsSession& s, std::string_view text);
  OpResult dispatch_ws_api(net::WsSession& s, const WsApiRequest& req, std::int64_t now_ms);
  [[nodiscard]] std::uint32_t rest_weight(RestEndpoint ep, const ParamList& p) const;
  [[nodiscard]] std::uint32_t ws_api_weight(std::string_view method, const ParamList& p) const;
  [[nodiscard]] OpResult rate_limit_error(std::int64_t now_ms, bool ws_api);
  [[nodiscard]] std::string rate_limits_json(std::int64_t now_ms);
  net::HttpServerConfig http_config() const;

  // ---- state --------------------------------------------------------------------------------
  SimServerConfig cfg_;
  std::vector<SymbolRuntime> symbols_;
  FeeModel fees_;
  std::int64_t start_mono_ns_ = 0;
  std::int64_t start_epoch_ns_ = 0;
  std::int64_t listen_mono_ns_ = 0;
  std::int64_t clock_offset_ms_ = 0;

  net::Reactor reactor_;
  std::unique_ptr<net::TlsContext> tls_ctx_;
  std::unique_ptr<net::HttpServer<net::PlainStream>> plain_;
  std::unique_ptr<net::HttpServer<TlsServerStream>> tls_;
  std::uint16_t port_ = 0;
  std::uint16_t tls_port_ = 0;
  std::string last_error_;

  std::unique_ptr<MatchingEngine> me_;
  std::unique_ptr<MdAggregator> agg_;
  std::vector<std::unique_ptr<MarketGenerator>> generators_;

  std::vector<Account> accounts_;
  OrderIndex orders_;
  std::map<std::string, ListenKey, std::less<>> listen_keys_;
  FixedWindowCounter weight_1m_{60'000};
  std::unordered_map<net::WsSession*, SessionState> sessions_;
  std::uint64_t next_token_ = 1;

  std::int64_t next_order_id_ = 1;
  std::uint64_t next_internal_ = 1;
  std::uint64_t trade_seq_ = 0;
  std::int64_t next_execution_id_ = 1;
  std::uint64_t next_generated_id_ = 1;
  std::uint64_t next_fault_order_ = 1;  // counter-orders of fill_open_order()

  // per-request context
  bool in_request_ = false;
  std::uint32_t request_delay_ms_ = 0;
  std::vector<std::pair<AccountId, std::string>> pending_events_;
  std::int64_t capture_order_id_ = 0;
  std::vector<CapturedFill> captured_fills_;
  std::string cancel_client_id_;
  bool amend_in_progress_ = false;

  FaultState faults_;
  // Every clientOrderId the account has had accepted, so a repeat is visible long after the first
  // order was forgotten (SimServerStats::duplicate_client_order_ids).
  std::unordered_set<std::string> seen_client_ids_;
  SimServerStats stats_;
  std::vector<Level> level_buf_;
  std::string md_scratch_;
  std::string md_wrapped_;

  std::thread thread_;
  std::thread::id thread_id_{};
  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};
};

}  // namespace fastmm::sim::server
