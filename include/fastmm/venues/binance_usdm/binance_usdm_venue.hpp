#pragma once
// BinanceUsdmVenue: the Binance USDⓈ-M perpetual futures connector (Demo Trading or production
// hosts; one-way position mode, HMAC or Ed25519 keys). Ed25519 keys log on to the WS API order
// connection once (session.logon) and send unsigned requests after that.
//
// Channels on one reactor thread:
//   md      <ws_url>/public/stream?streams=<sym>@depth@100ms/<sym>@bookTicker   (BinanceUsdmMdFeed)
//   ticker  <ws_url>/public/stream?streams=<sym>@bookTicker, md_ticker_conns copies (default 0):
//           the same bookTickers on more connections, the first copy of each update id wins
//   trades  <ws_url>/market/stream?streams=<sym>@aggTrade/<sym>@markPrice@1s   (the mark price
//           stream for perpetuals: mark, index and funding as PerpStateMsg)
//   user    <ws_private_url>/ws/<listenKey>   ORDER_TRADE_UPDATE, ACCOUNT_UPDATE, listenKeyExpired;
//           the listenKey comes from POST /fapi/v1/listenKey and is kept alive with PUT every
//           30 minutes (valid 60 minutes, "User Data Streams")
//   order   <ws_api_url>   order.place / order.cancel / order.modify, REST fallback
//   rest    <rest_url>     exchangeInfo, depth, time, listenKey, openOrders, positionRisk, REST
//                          orders (RestChannel on the reactor)
// cancel_all() uses an independent BlockingHttp connection (DELETE /fapi/v1/allOpenOrders).
//
// Reconciliation: on every user-stream connect and order-channel reconnect, GET /fapi/v1/userTrades
// per symbol replays the executions since the last one forwarded (Venue::request_executions), then
// GET /fapi/v1/openOrders and GET /fapi/v3/positionRisk become one ReconcileMsg Begin / OpenOrder*
// / Position* / End, with kExecutionsExact on the Begin when the replay was complete. Positions:
// the engine books fills; ACCOUNT_UPDATE positions are compared with the connector's own sum of
// forwarded fills once no fill or position event has arrived for position_settle_ms, and a
// PositionUpdateMsg corrects the engine only when they differ (liquidation, ADL, trades by other
// software). ACCOUNT_UPDATE and ORDER_TRADE_UPDATE are not ordered against each other, so
// forwarding every position event would double count fills.
//
// Funding: GET /fapi/v1/income?incomeType=FUNDING_FEE, account-wide, becomes one FundingMsg per
// payment on a subscribed symbol (tranId as its id). It runs with every reconciliation's execution
// replay and once a minute (its own ReplayScheduler and watermark), is retried like it, and runs a
// second after an ACCOUNT_UPDATE with reason FUNDING_FEE: that event names the symbol but has no
// id, so the income history is what is booked, once, whichever path found it first.
//
// Balances: after every open-order snapshot, and at most once a second after an ACCOUNT_UPDATE that
// names balances, GET /fapi/v3/account becomes one BalanceMsg snapshot: a row per asset (available,
// initial margin, wallet, margin balance, maintenance margin) and, in Multi-Assets Mode, the
// account row in USD (kAccount) from the totals. ACCOUNT_UPDATE B[] alone is not forwarded: it has
// the wallet balance but not the available balance or the margin, and a BalanceMsg replaces every
// amount. The mode comes from GET /fapi/v1/multiAssetsMargin at start-up and from
// ACCOUNT_CONFIG_UPDATE ai.j after that.
//
// Funding interval: markPriceUpdate has the rate and the next funding time but not the interval.
// load_reference_data() reads GET /fapi/v1/fundingInfo (public, weight 0) once, next to
// exchangeInfo: it lists the symbols whose interval was adjusted (fundingIntervalHours, 1, 4 or 8
// in 2026-09); every other symbol pays every 8 hours. A failed fetch logs a warning and leaves
// 8 hours. During the session the parser follows the next funding time: the step it takes at a
// funding is the interval in force (binance_usdm_md_parser.hpp).
//
// Margin mode is not managed: load_reference_data() logs the position mode, leverage, margin type
// and balances, and refuses to start in hedge mode. Two opt-in keys change the account first:
// one_way_mode (POST /fapi/v1/positionSide/dual, only with no position and no open order) and
// leverage (POST /fapi/v1/leverage per enabled symbol).
//
// What this connector shares with Binance Spot, and what it does not. Shared, because Binance
// documents one contract for both: request signing and the WS API frame
// (binance/binance_params.hpp), the market-data feed machinery
// (binance/binance_md_feed_base.hpp), the depth syncer (binance/binance_depth_sync.hpp, with
// futures `pu` chaining instead of Spot's U/u), the credentials and Ed25519 key loading
// (binance/binance_auth.hpp), the WS API response decoder, the REST error decoder and the
// trade-history parser and replayed fill (binance/binance_trade_history.hpp). Its own,
// because the protocols differ:
//   * endpoints and weights: /fapi/v1 and /fapi/v3 against /api/v3, a different depth-weight
//     table, exchangeInfo with contractType and no symbol filter;
//   * user stream: listenKey only (Spot picks between the WS API user stream and listenKey);
//   * market data: two connections (public + aggTrade/markPrice market) against Spot's one, no SBE;
//   * account: one-way position mode is checked at start-up, and ACCOUNT_UPDATE positions are
//     reconciled against the connector's own sum of forwarded fills (see above);
//   * reconciliation: openOrders *and* positionRisk, emitted as one snapshot when both replies
//     are in, against Spot's single openOrders reply;
//   * cancel_all(): Spot treats the -2011 "no open orders" reply as success, this one does not.
#include "fastmm/config/config.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/containers/recent_map.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/net/connection.hpp"
#include "fastmm/venues/binance/binance_auth.hpp"
#include "fastmm/venues/binance/binance_order_encoder.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_error_map.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_md_feed.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_order_encoder.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_rest_decoder.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_user_parser.hpp"
#include "fastmm/venues/blocking_http.hpp"
#include "fastmm/venues/connection_slot.hpp"
#include "fastmm/venues/dead_mans_switch.hpp"
#include "fastmm/venues/order_commands.hpp"
#include "fastmm/venues/rate_limiter.hpp"
#include "fastmm/venues/raw_recorder.hpp"
#include "fastmm/venues/reconcile_driver.hpp"
#include "fastmm/venues/replay_scheduler.hpp"
#include "fastmm/venues/rest_channel.hpp"
#include "fastmm/venues/venue.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fastmm::venues::binance_usdm {

struct BinanceUsdmVenueConfig {
  std::string name = "binance_usdm";
  std::string ws_url;          // stream root: wss://fstream.binance.com (Demo: demo-fstream)
  std::string ws_api_url;      // wss://ws-fapi.binance.com/ws-fapi/v1
  std::string rest_url;        // https://fapi.binance.com (Demo: https://demo-fapi.binance.com)
  std::string ws_private_url;  // user data root; empty = <ws_url>/private
  // Extra bookTicker-only connections next to the md one. Binance spreads WebSocket connections
  // over push servers that each lag now and then; the earliest copy of every update is pushed
  // (BasicBinanceMdFeed::set_ticker_dedup). Their state is logged, not reported: the md
  // connection alone decides whether the books are live.
  std::uint32_t md_ticker_conns = 0;
  binance::Credentials credentials;
  int recv_window_ms = kDefaultRecvWindowMs;
  bool insecure_tls = false;
  std::string ca_file;
  bool dry_run = false;  // public market data only: no user/order channels
  // A pool member ([venues.<x>] pool_of): the venue whose instruments this account trades; order,
  // user and REST sessions only, no market data (binance/binance_venue.hpp has the whole of it).
  VenueId pool_of{};
  // One of a pool's accounts: the IP's request weight is counted with the others' (share_ip).
  bool share_ip_weight = false;
  bool ws_order_api = true;  // false: REST order entry only
  bool emit_ack_from_response = true;
  bool position_from_account_update = true;  // correct the engine position (see above)
  bool cancel_on_order_channel_loss = true;
  // Venue-side dead man's switch (POST /fapi/v1/countdownCancelAll): the venue cancels every
  // open order of a symbol unless the connector refreshes its countdown. 0 disables it. This is
  // the only thing that clears quotes after a SIGKILL, an OOM kill or a dead host, so it is on
  // by default. The window is the exposure after such a kill; the refresh goes out every
  // window/3, costing IP weight 10 per symbol per refresh out of the 2400/minute budget
  // (60 s window, 4 symbols: 120 weight per minute, 5 %).
  std::int64_t dead_mans_switch_ms = 60'000;
  bool allow_offline_reference_data = false;
  // Account settings load_reference_data() may change; by default it changes none. one_way_mode:
  // an account in hedge mode is switched to one-way mode when it holds no position and no open
  // order (otherwise the start still fails). leverage > 0: every enabled symbol whose leverage
  // differs is set to it.
  bool one_way_mode = false;
  int leverage = 0;
  // Post-only orders are sent with timeInForce RPI instead of GTX (see the order encoder). The
  // venue does not modify an RPI order, so use it with supports_replace = false.
  bool post_only_rpi = false;
  bool supports_replace = true;  // order.modify
  // GET /fapi/v1/depth limit: 5, 10, 20, 50, 100, 500, 1000. 0: by the subscribed symbols
  // (auto_depth_limit): 1000 (weight 20) up to kDeepBookSymbols of them, else 100 (weight 5).
  int depth_limit = 0;
  std::uint32_t stale_ms = 2000;
  std::uint32_t dead_ms = 10'000;
  std::uint64_t max_lifetime_ms = 23ULL * 3600 * 1000;  // connections are cut at 24 h
  std::int64_t position_settle_ms = 1000;
  double rate_threshold = 0.9;
  std::int64_t min_snapshot_interval_ns = UsdmDepthSync::kDefaultMinInterval;
  std::string record_raw_dir;
  std::uint32_t http_timeout_ms = 5000;
  net::BackoffConfig backoff{};
};

// The depth a session of `symbols` snapshots with when depth_limit is 0 (BinanceUsdmVenueConfig).
inline constexpr std::size_t kDeepBookSymbols = 10;
// The most md_ticker_conns a venue opens.
inline constexpr std::size_t kMaxTickerConns = 8;
[[nodiscard]] inline int auto_depth_limit(std::size_t symbols) noexcept {
  return symbols <= kDeepBookSymbols ? 1000 : 100;
}

class BinanceUsdmVenue final : public Venue, private ReconcileHooks {
 public:
  BinanceUsdmVenue(VenueId id, BinanceUsdmVenueConfig cfg);
  ~BinanceUsdmVenue() override;

  // ---- Venue ---------------------------------------------------------------------------------
  [[nodiscard]] VenueId id() const noexcept override { return id_; }
  [[nodiscard]] std::string_view name() const noexcept override { return cfg_.name; }
  [[nodiscard]] VenueCaps caps() const noexcept override;
  Result<void, std::string> load_reference_data(InstrumentTable& instruments) override;
  [[nodiscard]] bool refused_account_settings() const noexcept override {
    return refused_account_settings_;
  }
  void attach(const SymbolTable& symbols,
              const InstrumentTable& instruments,
              EventSink& md_sink,
              EventSink& order_sink,
              MsgRing* outbound) override;
  void connect(net::Reactor& reactor) override;
  void disconnect() override;
  void subscribe(std::span<const InstrumentId> instruments) override;
  void on_timer(std::int64_t now_ns) override;
  void resync_books() override;
  void on_wake() override;
  void send_now(std::span<const EventHeader* const> batch) override;
  void request_open_orders() override;
  bool request_executions(std::int64_t since_venue_ms = 0) override;
  void resume_executions(std::int64_t since_venue_ms,
                         const std::vector<std::string>& known) override;
  void resume_trade_ids(
      const std::vector<std::pair<InstrumentId, std::int64_t>>& next_ids) override;
  void resume_known_trade_ids(
      const std::vector<std::pair<InstrumentId, std::vector<std::int64_t>>>& known) override;
  bool cancel_all() override;
  [[nodiscard]] VenueStatus status() const noexcept override;

  // ---- introspection (tests / stats) --------------------------------------------------------
  [[nodiscard]] const BinanceUsdmMdFeed* md_feed() const noexcept { return md_feed_.get(); }
  [[nodiscard]] const RateLimiter& rate_limiter() const noexcept { return rate_; }
  [[nodiscard]] std::int64_t clock_offset_ms() const noexcept { return clock_offset_ms_; }
  [[nodiscard]] const BinanceUsdmVenueConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] bool fatal() const noexcept { return fatal_; }
  // Orders the connector still keeps a shadow for (tests: a lost terminal event leaks one).
  [[nodiscard]] std::size_t shadow_count() const noexcept { return shadows_.size(); }
  [[nodiscard]] std::int64_t venue_time_ms() const noexcept;

 private:
  enum class Channel : std::uint8_t { Md = 0, Trades = 1, User = 2, Order = 3 };

  struct MdHandler {
    BinanceUsdmVenue* v;
    void on_state(net::ConnState s) { v->on_md_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_md_text(t, ts, Channel::Md); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_md_open(); }
  };
  struct TickerHandler {
    BinanceUsdmVenue* v;
    void on_state(net::ConnState s) { v->on_ticker_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_ticker_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() {}
  };
  struct TradesHandler {
    BinanceUsdmVenue* v;
    void on_state(net::ConnState s) { v->on_trades_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_md_text(t, ts, Channel::Trades); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() {}
  };
  struct UserHandler {
    BinanceUsdmVenue* v;
    void on_state(net::ConnState s) { v->on_user_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_user_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() {}
  };
  struct OrderHandler {
    BinanceUsdmVenue* v;
    void on_state(net::ConnState s) { v->on_order_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_order_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_order_open(); }
  };
  friend struct MdHandler;
  friend struct TradesHandler;
  friend struct TickerHandler;
  friend struct UserHandler;
  friend struct OrderHandler;

  // Per instrument: the position the engine holds from forwarded fills, and the last venue view.
  struct PositionCheck {
    Qty tracked{};
    Qty venue{};
    Price venue_avg{};
    std::int64_t last_event_ns = 0;
    bool pending = false;  // an ACCOUNT_UPDATE position has not been compared yet
  };

  // channel callbacks (reactor thread)
  void on_md_state(net::ConnState s);
  void on_md_text(std::string_view t, std::int64_t ts, Channel ch);
  void on_md_open();
  void on_trades_state(net::ConnState s);
  void on_ticker_state(net::ConnState s);
  void on_ticker_text(std::string_view t, std::int64_t ts);
  void on_user_state(net::ConnState s);
  void on_user_text(std::string_view t, std::int64_t ts);
  void on_order_state(net::ConnState s);
  void on_order_text(std::string_view t, std::int64_t ts);
  void on_order_open();
  void send_logon();

  // helpers
  void open_md();
  void open_trades();
  void open_tickers();
  void close_tickers();
  void open_user();
  void open_order();
  void open_rest();
  void report_channel_state(Channel ch, ConnState state, std::int32_t reason = 0);
  void drain_outbound();
  // Encodes and writes the orders `ring` holds (the outbound MsgRing or an OutboundBatch).
  template <class Ring>
  void write_orders(Ring& ring);
  void send_command(const OrderCommand& cmd);
  void send_command_rest(const OrderCommand& cmd, const OrderShadow* shadow);
  // uncork() failed: the batch never left, so its orders are rejected (see BatchedOrders).
  void fail_batch();
  void handle_ws_api_response(const binance::WsApiResponse& r);
  void handle_order_response(RequestKind kind, ClientOrderId id, const binance::WsApiResponse& r);
  void handle_rest_order_response(const OrderCommand& cmd, const net::HttpResponse& r);
  void trip_venue_kill(KillReason reason);
  void apply_action(VenueAction action,
                    int code,
                    std::string_view msg,
                    std::int64_t retry_after_ms);
  void refuse(const OrderCommand& cmd, RejectReason reason, std::string_view text);
  void request_snapshot(InstrumentId id);
  void request_server_time();
  // The rate limiter's windows on the venue's clock (clock_offset_ms_), after each sync.
  void align_rate_windows() noexcept;
  void request_listen_key();
  void keepalive_listen_key();
  void cancel_all_async();
  // Arms or refreshes countdownCancelAll on every subscribed symbol: one CountdownDriver round.
  // stop_countdown_blocking() stops it.
  void send_countdown_cancel_all(std::int64_t countdown_ms);
  void stop_countdown_blocking();
  // ReconcileHooks: GET /fapi/v1/openOrders + GET /fapi/v3/positionRisk, the snapshot once both
  // replies are in.
  bool fetch_snapshot(std::uint64_t generation) override;
  bool replay_executions() override;
  void shadow_ids(std::vector<SentShadow>& out) override;
  void drop_shadow(ClientOrderId id) override;
  void on_reconcile_reply(std::uint64_t generation, bool orders, const net::HttpResponse& r);
  // ReconcileHooks: GET /fapi/v3/account.
  bool fetch_balances(std::uint64_t generation) override;
  void on_account(std::uint64_t generation, const net::HttpResponse& r);
  // Execution replay (ReplayScheduler): GET /fapi/v1/userTrades for one subscribed instrument,
  // and a row of it forwarded as a replayed fill.
  [[nodiscard]] bool replay_ready() const noexcept;
  bool query_executions(const ReplayQuery& q);
  bool emit_execution(std::size_t stream, const binance::MyTradeRow& t);
  // GET /fapi/v1/order?orderId= for a replayed execution whose order order_ids_ does not name.
  bool lookup_order(const ReplayLookup& l);
  // Funding (ReplayScheduler): GET /fapi/v1/income?incomeType=FUNDING_FEE, and a row forwarded
  // as a funding payment.
  bool query_funding(const ReplayQuery& q);
  bool emit_funding_row(const IncomeRecord& row);
  // A REST failure of a replay query: the error mapping's action, the log line.
  // `then`: what follows the failure, for the log: a query is asked again, an order lookup leaves
  // its fill naming no order yet.
  void replay_query_failed(std::string_view what,
                           const net::HttpResponse& r,
                           std::string_view then = "asked again");
  [[nodiscard]] std::size_t subscribed_slot(InstrumentId id) const noexcept;
  void remember_order_id(std::int64_t order_id, ClientOrderId id) noexcept;
  // Both replies in: decodes them into the driver's rows (false when one does not parse).
  bool snapshot_rows();
  void on_account_position(const PositionUpdateMsg& m);
  void check_positions(std::int64_t now);
  void publish_status() noexcept;
  // An order the shadow table had no room for goes back as OrderTableFull.
  void refuse_untracked(const OrderCommand& cmd);
  void note_rate_headers(const net::HttpResponse& r);
  [[nodiscard]] ClientOrderId current_id(ClientOrderId link) const noexcept;
  void forget_order(ClientOrderId id) noexcept;
  [[nodiscard]] Qty engine_cum(ClientOrderId id, Qty venue_cum) const noexcept;
  [[nodiscard]] std::int64_t now_ns() const noexcept { return net::Reactor::now_ns(); }
  static void snapshot_requester(void* ctx, InstrumentId id) noexcept {
    static_cast<BinanceUsdmVenue*>(ctx)->request_snapshot(id);
  }
  [[nodiscard]] InstrumentId instrument_of(std::string_view symbol) const noexcept;
  [[nodiscard]] bool pool_member() const noexcept { return cfg_.pool_of.valid(); }
  [[nodiscard]] std::string api_headers() const;
  // A signed request's target, stamped and signed when it is written (binance::signed_rest_target).
  [[nodiscard]] std::function<std::string()> signed_target(const RestRequest& rr) const {
    return binance::signed_rest_target(signer_, rr, [this] { return venue_time_ms(); });
  }
  // A replay's query goes out only while the REST connection holds fewer requests than this: the
  // rest wait for a reply or a housekeeping tick, not in the connection's queue, where an order
  // or a snapshot sent meanwhile would wait behind them all.
  static constexpr std::size_t kReplayQueueDepth = 4;
  [[nodiscard]] bool replay_room() const noexcept {
    return rest_ == nullptr || rest_->queued() < kReplayQueueDepth;
  }
  [[nodiscard]] std::string stream_root() const;
  [[nodiscard]] std::string private_root() const;
  [[nodiscard]] net::ConnectionConfig ws_config(const std::string& url,
                                                std::uint32_t min_dead_ms) const;
  Result<void, std::string> apply_exchange_info(const HttpReply& reply,
                                                const std::vector<Instrument*>& mine,
                                                const std::vector<std::string>& wanted);
  std::string account_checks(const std::vector<Instrument*>& mine);
  void load_funding_intervals(const std::vector<Instrument*>& mine,
                              const std::vector<std::string>& wanted);
  [[nodiscard]] Duration funding_interval_of(InstrumentId id) const noexcept;

  VenueId id_;
  BinanceUsdmVenueConfig cfg_;
  VenueId md_venue_;  // the instruments' venue: id_, or the pool's primary
  Signer signer_;
  const SymbolTable* symbols_ = nullptr;
  const InstrumentTable* instruments_ = nullptr;
  EventSink* md_sink_ = nullptr;
  EventSink* order_sink_ = nullptr;
  MsgRing* outbound_ = nullptr;
  net::Reactor* reactor_ = nullptr;

  std::unique_ptr<BinanceUsdmMdFeed> md_feed_;
  std::unique_ptr<BinanceUsdmUserParser> user_parser_;
  std::unique_ptr<BinanceUsdmOrderEncoder> encoder_;
  std::unique_ptr<binance::BinanceWsApiDecoder> ws_api_decoder_;
  std::unique_ptr<RestChannel> rest_;
  MdHandler md_handler_{this};
  TradesHandler trades_handler_{this};
  TickerHandler ticker_handler_{this};
  UserHandler user_handler_{this};
  OrderHandler order_handler_{this};
  ConnectionSlot<MdHandler> md_conn_;
  ConnectionSlot<TradesHandler> trades_conn_;
  std::array<ConnectionSlot<TickerHandler>, kMaxTickerConns> ticker_conns_;
  ConnectionSlot<UserHandler> user_conn_;
  ConnectionSlot<OrderHandler> order_conn_;
  RateLimiter rate_;
  OpenHashMap<ClientOrderId, OrderShadow, kShadowSlots> shadows_;
  ShadowOverflow shadow_overflow_;
  OpenHashMap<ClientOrderId, ClientOrderId, kShadowSlots> aliases_;  // venue link id -> engine id
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
  char request_buf_[kMaxRequestBytes];
  RawRecorder raw_md_;
  RawRecorder raw_trades_;
  RawRecorder raw_user_;
  RawRecorder raw_order_;

  std::vector<InstrumentId> subscribed_;

  int depth_limit_ = 1000;  // the snapshot depth in use (cfg_.depth_limit or auto_depth_limit)
  // GET /fapi/v1/fundingInfo's interval of each listed instrument (load_reference_data()).
  std::vector<std::pair<InstrumentId, Duration>> funding_intervals_;
  CountdownDriver dms_;
  std::array<PositionCheck, kMaxInstruments> positions_{};
  // The snapshot's two REST replies, collected before anything is emitted.
  int snapshot_replies_ = 0;
  bool snapshot_failed_ = false;
  std::string orders_body_;
  std::string positions_body_;
  // Venue order id -> the engine id it was acknowledged for: userTrades names the order by orderId
  // only. One not here (a restarted process, an answer that never came) the execution replay asks
  // the venue for (lookup_order), and keeps here.
  RecentMap<std::uint64_t, ClientOrderId, 8192> order_ids_;
  // Execution replay, one stream per subscribed_ instrument: from the trade id after the last one
  // read (fromId), else from the time watermark.
  ReplayScheduler<binance::MyTradeRow> exec_replay_;
  // Orders sent and order events heard per instrument: a sweep skips the quiet ones.
  OrderActivity activity_;
  // Funding, one account-wide stream.
  ReplayScheduler<IncomeRecord> funding_replay_;
  // The first trade id per instrument an earlier session left off at (resume_trade_ids): handed
  // to exec_replay_ at connect(), once subscribed_ gives the instruments their streams.
  std::vector<std::pair<InstrumentId, std::int64_t>> resume_from_ids_;
  // The trade ids at or after those starts that are booked already (resume_known_trade_ids).
  std::vector<std::pair<InstrumentId, std::vector<std::int64_t>>> resume_known_ids_;
  std::string listen_key_;
  std::int64_t listen_key_refresh_ns_ = 0;
  std::int64_t listen_key_retry_ns_ = 0;
  bool listen_key_pending_ = false;
  std::atomic<std::int64_t> clock_offset_ms_{0};  // read by cancel_all() from any thread
  std::int64_t clock_sync_ns_ = 0;
  bool time_request_pending_ = false;
  bool clock_resync_wanted_ = false;
  bool fatal_ = false;
  bool session_logged_on_ = false;
  bool venue_kill_sent_ = false;
  bool connected_ = false;
  bool rest_hard_stopped_ = false;
  // Multi-Assets Mode: the margin is the account's, in USD (the kAccount balance row).
  bool multi_assets_ = false;
  bool refused_account_settings_ = false;  // hedge mode, or a setting one_way_mode/leverage failed
  ConnState md_state_ = ConnState::Disconnected;
  ConnState trades_state_ = ConnState::Disconnected;
  ConnState user_state_ = ConnState::Disconnected;
  ConnState order_state_ = ConnState::Disconnected;
  bool order_was_live_ = false;
  bool user_was_live_ = false;
  SentWatermark sent_;
  BatchedOrders batch_;  // orders written into the corked order connection
  ReconcileDriver reconcile_{*this, sent_};
  net::TimerId housekeeping_timer_ = net::kInvalidTimer;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);

  WireLatencyRecorder wire_;
  VenueStatus stats_{};
  Seqlocked<VenueStatus> published_{};
};

// Builds a BinanceUsdmVenueConfig from a [venues.<name>] section. Extra keys: ws_private_url,
// order_api ("ws" | "rest"), depth_limit, stale_ms, dead_ms, position_from_account_update,
// allow_offline_reference_data, cancel_on_order_channel_loss, emit_ack_from_response,
// one_way_mode, leverage. Throws std::invalid_argument when key_type = "ed25519" has no parsable
// private key (live sessions) or leverage is outside 0 to 125.
BinanceUsdmVenueConfig make_binance_usdm_config(const VenueSection& v, bool dry_run);

}  // namespace fastmm::venues::binance_usdm
