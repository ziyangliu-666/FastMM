#pragma once
// BinanceUsdmVenue: the Binance USDⓈ-M perpetual futures connector (Demo Trading or production
// hosts; one-way position mode, HMAC or Ed25519 keys). Ed25519 keys log on to the WS API order
// connection once (session.logon) and send unsigned requests after that.
//
// Channels on one reactor thread:
//   md      <ws_url>/public/stream?streams=<sym>@depth@100ms/<sym>@bookTicker   (BinanceUsdmMdFeed)
//   trades  <ws_url>/market/stream?streams=<sym>@aggTrade
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
// payment on a subscribed symbol (tranId as its id). It runs with every execution replay (connect,
// reconciliation, the periodic sweep) from its own watermark, is retried like it, and runs a second
// after an ACCOUNT_UPDATE with reason FUNDING_FEE: that event names the symbol but has no id, so
// the income history is what is booked, once, whichever path found it first.
//
// Leverage and margin mode are not managed: load_reference_data() logs the position mode,
// leverage, margin type and balances, and refuses to start in hedge mode.
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
//   * market data: two connections (public + aggTrade market) against Spot's one, and no SBE;
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
#include "fastmm/venues/binance_usdm/binance_usdm_user_parser.hpp"
#include "fastmm/venues/connection_slot.hpp"
#include "fastmm/venues/dead_mans_switch.hpp"
#include "fastmm/venues/order_commands.hpp"
#include "fastmm/venues/rate_limiter.hpp"
#include "fastmm/venues/raw_recorder.hpp"
#include "fastmm/venues/rest_channel.hpp"
#include "fastmm/venues/venue.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fastmm::venues::binance_usdm {

struct BinanceUsdmVenueConfig {
  std::string name = "binance_usdm";
  std::string ws_url;          // stream root: wss://fstream.binance.com (Demo: demo-fstream)
  std::string ws_api_url;      // wss://ws-fapi.binance.com/ws-fapi/v1
  std::string rest_url;        // https://fapi.binance.com (Demo: https://demo-fapi.binance.com)
  std::string ws_private_url;  // user data root; empty = <ws_url>/private
  binance::Credentials credentials;
  int recv_window_ms = kDefaultRecvWindowMs;
  bool insecure_tls = false;
  std::string ca_file;
  bool dry_run = false;      // public market data only: no user/order channels
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
  bool supports_replace = true;  // order.modify
  int depth_limit = 1000;        // GET /fapi/v1/depth limit: 5, 10, 20, 50, 100, 500, 1000
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

class BinanceUsdmVenue final : public Venue {
 public:
  BinanceUsdmVenue(VenueId id, BinanceUsdmVenueConfig cfg);
  ~BinanceUsdmVenue() override;

  // ---- Venue ---------------------------------------------------------------------------------
  [[nodiscard]] VenueId id() const noexcept override { return id_; }
  [[nodiscard]] std::string_view name() const noexcept override { return cfg_.name; }
  [[nodiscard]] VenueCaps caps() const noexcept override;
  Result<void, std::string> load_reference_data(InstrumentTable& instruments) override;
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
  bool cancel_all() override;
  [[nodiscard]] VenueStatus status() const noexcept override;

  // ---- introspection (tests / stats) --------------------------------------------------------
  [[nodiscard]] const BinanceUsdmMdFeed* md_feed() const noexcept { return md_feed_.get(); }
  [[nodiscard]] const RateLimiter& rate_limiter() const noexcept { return rate_; }
  [[nodiscard]] std::int64_t clock_offset_ms() const noexcept { return clock_offset_ms_; }
  [[nodiscard]] const BinanceUsdmVenueConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] bool fatal() const noexcept { return fatal_; }
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
  // One reconciliation: both REST replies are collected, then emitted together.
  struct ReconcileState {
    std::uint64_t generation = 0;
    bool in_flight = false;
    bool again = false;
    int replies = 0;
    bool failed = false;
    ClientOrderId watermark{};
    std::string orders_body;
    std::string positions_body;
  };

  // channel callbacks (reactor thread)
  void on_md_state(net::ConnState s);
  void on_md_text(std::string_view t, std::int64_t ts, Channel ch);
  void on_md_open();
  void on_trades_state(net::ConnState s);
  void on_user_state(net::ConnState s);
  void on_user_text(std::string_view t, std::int64_t ts);
  void on_order_state(net::ConnState s);
  void on_order_text(std::string_view t, std::int64_t ts);
  void on_order_open();
  void send_logon();

  // helpers
  void open_md();
  void open_trades();
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
  void request_listen_key();
  void keepalive_listen_key();
  void cancel_all_async();
  // Arms or refreshes countdownCancelAll on every subscribed symbol. `countdown_ms` 0 stops it.
  void send_countdown_cancel_all(std::int64_t countdown_ms);
  void stop_countdown_blocking();
  // GET /fapi/v1/openOrders + positionRisk, once any execution replay before it has finished.
  void send_open_orders();
  void on_reconcile_reply(std::uint64_t generation, bool orders, const net::HttpResponse& r);
  // GET /fapi/v1/userTrades for one subscribed instrument; `emit_executions` turns the reply into
  // replayed fills and `finish_execution_replay` releases the snapshot when the last one is in.
  bool request_executions_for(InstrumentId id);
  void emit_executions(InstrumentId id, std::string_view json, std::int64_t window_end_ms);
  void finish_execution_replay(bool ok);
  // GET /fapi/v1/income?incomeType=FUNDING_FEE from funding_since_ms_; `emit_funding_rows` turns
  // the reply into funding payments and moves the watermark.
  void request_funding();
  void emit_funding_rows(std::string_view json, std::int64_t window_end_ms, bool complete);
  [[nodiscard]] std::size_t exec_slot(InstrumentId id) const noexcept;
  void remember_order_id(std::int64_t order_id, ClientOrderId id) noexcept;
  void emit_reconcile();
  void on_account_position(const PositionUpdateMsg& m);
  void check_positions(std::int64_t now);
  void publish_status() noexcept;
  void note_rate_headers(const net::HttpResponse& r);
  [[nodiscard]] ClientOrderId current_id(ClientOrderId link) const noexcept;
  void forget_order(ClientOrderId id) noexcept;
  [[nodiscard]] Qty engine_cum(ClientOrderId id, Qty venue_cum) const noexcept;
  [[nodiscard]] std::int64_t now_ns() const noexcept { return net::Reactor::now_ns(); }
  static void snapshot_requester(void* ctx, InstrumentId id) noexcept {
    static_cast<BinanceUsdmVenue*>(ctx)->request_snapshot(id);
  }
  [[nodiscard]] InstrumentId instrument_of(std::string_view symbol) const noexcept;
  [[nodiscard]] std::string api_headers() const;
  [[nodiscard]] std::string stream_root() const;
  [[nodiscard]] std::string private_root() const;
  [[nodiscard]] net::ConnectionConfig ws_config(const std::string& url,
                                                std::uint32_t min_dead_ms) const;
  std::string account_checks(const std::vector<Instrument*>& mine);

  VenueId id_;
  BinanceUsdmVenueConfig cfg_;
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
  UserHandler user_handler_{this};
  OrderHandler order_handler_{this};
  ConnectionSlot<MdHandler> md_conn_;
  ConnectionSlot<TradesHandler> trades_conn_;
  ConnectionSlot<UserHandler> user_conn_;
  ConnectionSlot<OrderHandler> order_conn_;
  RateLimiter rate_;
  OpenHashMap<ClientOrderId, OrderShadow, 8192> shadows_;
  OpenHashMap<ClientOrderId, ClientOrderId, 8192> aliases_;  // venue link id -> engine id
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
  char request_buf_[kMaxRequestBytes];
  RawRecorder raw_md_;
  RawRecorder raw_trades_;
  RawRecorder raw_user_;
  RawRecorder raw_order_;

  std::vector<InstrumentId> subscribed_;
  CountdownSwitch dms_;
  std::array<PositionCheck, kMaxInstruments> positions_{};
  ReconcileState reconcile_;
  std::int64_t reconcile_retry_ns_ = 0;
  bool oo_wanted_ = false;  // a snapshot is waiting for the execution replay
  // Execution replay (GET /fapi/v1/userTrades), parallel to subscribed_: the trade id to ask from
  // next (0 before this connector has forwarded one), else the venue time to ask from.
  // exec_since_ms_ seeds exec_start_ms_ and starts at connect() or resume_executions().
  std::vector<std::int64_t> exec_from_id_;
  std::vector<std::int64_t> exec_start_ms_;
  std::int64_t exec_since_ms_ = 0;
  // Venue order id -> the engine id it was acknowledged for: userTrades names the order by orderId
  // only. A restarted session's orders are not in it and reach the position as unknown fills.
  RecentMap<std::uint64_t, ClientOrderId, 8192> order_ids_;
  std::unordered_set<std::string> known_exec_ids_;  // booked by an earlier session
  // The first trade id per instrument an earlier session left off at (resume_trade_ids): moved
  // into exec_from_id_ by the next replay, once subscribed_ gives the instruments their slots.
  std::vector<std::pair<InstrumentId, std::int64_t>> resume_from_ids_;
  std::uint64_t exec_generation_ = 0;  // replies of an abandoned replay are ignored
  std::size_t exec_pending_ = 0;       // userTrades replies still outstanding
  bool exec_replay_ok_ = true;
  bool exec_replay_active_ = false;
  bool exec_snapshot_exact_ = false;  // stamp kExecutionsExact on the next snapshot's Begin
  bool exec_retry_wanted_ = false;    // the last replay was incomplete: ask again from on_timer
  std::int64_t exec_last_ns_ = 0;     // when the last replay started (the periodic one)
  std::int64_t exec_retry_ns_ = 0;
  // Funding replay: the venue time to ask from (inclusive), the tranIds forwarded at that time,
  // and a query a user-stream funding event asked for (reactor time; 0 none).
  std::int64_t funding_since_ms_ = 0;
  std::unordered_set<std::int64_t> funding_edge_ids_;
  bool funding_active_ = false;
  bool funding_retry_wanted_ = false;
  std::int64_t funding_retry_ns_ = 0;
  std::int64_t funding_due_ns_ = 0;
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
  ConnState md_state_ = ConnState::Disconnected;
  ConnState trades_state_ = ConnState::Disconnected;
  ConnState user_state_ = ConnState::Disconnected;
  ConnState order_state_ = ConnState::Disconnected;
  bool order_was_live_ = false;
  SentWatermark sent_;
  BatchedOrders batch_;  // orders written into the corked order connection
  // Open-order snapshot, decoded in full before anything reaches the engine.
  std::vector<ReconcileMsg> reconcile_records_;
  net::TimerId housekeeping_timer_ = net::kInvalidTimer;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);

  WireLatencyRecorder wire_;
  VenueStatus stats_{};
  Seqlocked<VenueStatus> published_{};
};

// Builds a BinanceUsdmVenueConfig from a [venues.<name>] section. Extra keys: ws_private_url,
// order_api ("ws" | "rest"), depth_limit, stale_ms, dead_ms, position_from_account_update,
// allow_offline_reference_data, cancel_on_order_channel_loss, emit_ack_from_response. Throws
// std::invalid_argument when key_type = "ed25519" has no parsable private key (live sessions).
BinanceUsdmVenueConfig make_binance_usdm_config(const VenueSection& v, bool dry_run);

}  // namespace fastmm::venues::binance_usdm
