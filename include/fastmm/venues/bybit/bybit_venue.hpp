#pragma once
// BybitVenue: the Bybit v5 connector (6.5), for spot or for linear (USDT- and USDC-margined)
// perpetuals: `category` picks one per instance (bybit_category.hpp).
//
// Channels on one reactor thread (URLs: https://bybit-exchange.github.io/docs/v5/ws/connect):
//   md       wss://stream-testnet.bybit.com/v5/public/spot   orderbook.<depth> / orderbook.1 /
//            (.../v5/public/linear)                          publicTrade (BybitMdFeed)
//   private  wss://stream-testnet.bybit.com/v5/private       op auth, then order / execution /
//            wallet (spot) or position (linear) topics, and dcp.spot / dcp.future when armed
//   trade    wss://stream-testnet.bybit.com/v5/trade         op auth, then order.create /
//            order.amend / order.cancel; REST fallback
//   rest     https://api-testnet.bybit.com                   market/time, order/realtime,
//            order/cancel-all, execution/list, position/list, REST order entry (RestChannel)
// Every WebSocket channel sends {"op":"ping"} every 20 s (connect page, "How to Send the
// Heartbeat Packet"). cancel_all() uses an independent BlockingHttp connection (6.7).
//
// Linear. instruments-info must say contractType LinearPerpetual: the instrument becomes a
// perpetual, multiplier 1 (qty is in the base coin), reduce-only capable, base = baseCoin and
// quote = settleCoin, which is what [accounting] settles it in. With keys, load_reference_data()
// reads GET /v5/position/list per symbol and refuses a symbol in hedge mode (a row with positionIdx
// 1 or 2; refused_account_settings()). Every reconciliation reads the open orders and the positions
// per settle coin after the execution replay and emits them as one Begin / OpenOrder* / Position* /
// End, a subscribed symbol absent from the list being flat. Between reconciliations the `position`
// topic is compared, as Binance USD-M compares ACCOUNT_UPDATE, with the connector's sum of the
// fills it forwarded once neither has changed for kPositionSettleMs, and a PositionUpdateMsg
// corrects the engine only when they differ (liquidation, ADL, another client). The ticker topic
// (mark price, funding) is not subscribed: no engine message carries it, and funding is not booked.
//
// Amend (Replace) keeps the venue's orderLinkId: the engine gets an ack for its new client
// id and later order/execution events for the original orderLinkId are translated to it.
#include "fastmm/config/config.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/net/connection.hpp"
#include "fastmm/venues/bybit/bybit_auth.hpp"
#include "fastmm/venues/bybit/bybit_category.hpp"
#include "fastmm/venues/bybit/bybit_error_map.hpp"
#include "fastmm/venues/bybit/bybit_md_feed.hpp"
#include "fastmm/venues/bybit/bybit_order_encoder.hpp"
#include "fastmm/venues/bybit/bybit_private_parser.hpp"
#include "fastmm/venues/bybit/bybit_rest_decoder.hpp"
#include "fastmm/venues/connection_slot.hpp"
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

namespace fastmm::venues::bybit {

struct BybitVenueConfig {
  std::string name = "bybit";
  BybitCategory category = BybitCategory::Spot;
  std::string ws_public_url;   // wss://stream-testnet.bybit.com/v5/public/spot
  std::string ws_private_url;  // wss://stream-testnet.bybit.com/v5/private
  std::string ws_trade_url;    // wss://stream-testnet.bybit.com/v5/trade
  std::string rest_url;        // https://api-testnet.bybit.com
  Credentials credentials;
  int recv_window_ms = kDefaultRecvWindowMs;
  bool insecure_tls = false;
  std::string ca_file;
  bool dry_run = false;
  bool ws_order_api = true;
  bool emit_ack_from_response = true;
  bool position_from_wallet = false;  // spot
  bool position_from_stream = true;   // linear: correct the engine from the position topic
  bool cancel_on_order_channel_loss = true;
  // Bybit's Disconnect-Cancel-All window, seconds; 0 disables it. Bybit accepts [3, 300] and
  // defaults the account setting to 10. Off here by default because DCP is not self-serve: the
  // docs say it "is only available for Ins clients" and has to be enabled by an account manager
  // first, so arming it on an ordinary account only produces an error on every connect. Set it
  // once the account has it and the connector will arm it (product SPOT, or DERIVATIVES for
  // linear) and subscribe the `dcp.spot` / `dcp.future` topic that DCP needs to fire at all.
  int dead_mans_switch_s = 0;
  bool allow_offline_reference_data = false;
  bool supports_replace = true;
  int depth = 50;
  std::uint32_t stale_ms = 2000;
  std::uint32_t dead_ms = 30'000;
  std::uint32_t ping_interval_ms = 20'000;
  std::int64_t auth_expires_ms = 10'000;  // expires = venue time + this
  std::uint32_t orders_per_second =
      10;  // spot per UID: create 20/s, amend 10/s (docs/v5/rate-limit)
  double rate_threshold = 0.9;
  std::string record_raw_dir;
  std::uint32_t http_timeout_ms = 5000;
  net::BackoffConfig backoff{};
};

class BybitVenue final : public Venue {
 public:
  BybitVenue(VenueId id, BybitVenueConfig cfg);
  ~BybitVenue() override;

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
  bool cancel_all() override;
  [[nodiscard]] VenueStatus status() const noexcept override;

  [[nodiscard]] const BybitMdFeed* md_feed() const noexcept { return md_feed_.get(); }
  [[nodiscard]] const BybitVenueConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] bool fatal() const noexcept { return fatal_; }
  [[nodiscard]] std::int64_t clock_offset_ms() const noexcept { return clock_offset_ms_.load(); }
  [[nodiscard]] std::int64_t venue_time_ms() const noexcept;

 private:
  struct MdHandler {
    BybitVenue* v;
    void on_state(net::ConnState s) { v->on_md_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_md_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_md_open(); }
  };
  struct PrivateHandler {
    BybitVenue* v;
    void on_state(net::ConnState s) { v->on_private_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_private_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_private_open(); }
  };
  struct TradeHandler {
    BybitVenue* v;
    void on_state(net::ConnState s) { v->on_trade_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_trade_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() {}
  };

  void on_md_state(net::ConnState s);
  void on_md_text(std::string_view t, std::int64_t ts);
  void on_md_open();
  void on_private_state(net::ConnState s);
  void on_private_text(std::string_view t, std::int64_t ts);
  void on_private_open();
  void on_trade_state(net::ConnState s);
  void on_trade_text(std::string_view t, std::int64_t ts);

  void open_rest();
  void open_md();
  void open_private();
  void open_trade();
  net::ConnectionConfig ws_config(const std::string& url,
                                  bool manual_auth,
                                  bool manual_subscribe) const;
  void send_auth(ConnectionSlot<PrivateHandler>* priv, ConnectionSlot<TradeHandler>* trade);
  void drain_outbound();
  // Encodes and writes the orders `ring` holds (the outbound MsgRing or an OutboundBatch).
  template <class Ring>
  void write_orders(Ring& ring);
  void send_command(const OrderCommand& cmd);
  void send_command_rest(const OrderCommand& cmd, const OrderShadow* shadow);
  // uncork() failed: the batch never left, so its orders are rejected (see BatchedOrders).
  void fail_batch();
  void handle_order_response(RequestKind kind, ClientOrderId id, const TradeResponse& r);
  void handle_rest_order_response(const OrderCommand& cmd, const net::HttpResponse& r);
  // Sends ControlCommand::TripVenueKill to the engine, once per session.
  void trip_venue_kill(KillReason reason);
  void apply_action(VenueAction action,
                    int code,
                    std::string_view msg,
                    std::int64_t reset_epoch_ms);
  void request_resubscribe(InstrumentId id);
  void request_server_time();
  void cancel_all_async();
  // POST /v5/order/disconnected-cancel-all, once per session after the private channel is up.
  void set_dcp();
  // Requests one page of GET /v5/order/realtime; the reply reads the next page or, on the last
  // one, emits the whole snapshot (emit_reconcile). Nothing is emitted unless every page parsed.
  void request_open_orders_page(const std::string& cursor);
  // Linear: one page of GET /v5/position/list for settle_coins_[reconcile_coin_]; the last page of
  // the last coin emits the snapshot.
  void request_positions_page(const std::string& cursor);
  // Linear, start-up: GET /v5/position/list per symbol; an error when a symbol is in hedge mode or
  // its mode cannot be read.
  std::string check_position_mode(const std::vector<Instrument*>& mine);
  // Linear: compares the position topic with the forwarded fills (see the header comment).
  void check_positions(std::int64_t now);
  void note_fill(const OrderFillMsg& f) noexcept;
  // Emits the open-order snapshot request itself, once any execution replay before it finished.
  void send_open_orders();
  void emit_reconcile();
  // Execution replay (GET /v5/execution/list): one window of at most 7 days at a time, paged
  // with nextPageCursor; a window's rows are emitted oldest first once its last page is in.
  void start_execution_window();
  void request_executions_page(const std::string& cursor);
  void on_executions_window_done();
  void emit_executions();
  void finish_execution_replay(bool ok);
  void publish_status() noexcept;
  void note_rate_headers(const net::HttpResponse& r);
  void forget_order(ClientOrderId id) noexcept;
  [[nodiscard]] ClientOrderId current_id(ClientOrderId link) const noexcept;
  [[nodiscard]] std::int64_t now_ns() const noexcept { return net::Reactor::now_ns(); }
  static void resubscribe_requester(void* ctx, InstrumentId id) noexcept {
    static_cast<BybitVenue*>(ctx)->request_resubscribe(id);
  }

  VenueId id_;
  BybitVenueConfig cfg_;
  Signer signer_;
  const SymbolTable* symbols_ = nullptr;
  const InstrumentTable* instruments_ = nullptr;
  EventSink* md_sink_ = nullptr;
  EventSink* order_sink_ = nullptr;
  MsgRing* outbound_ = nullptr;
  net::Reactor* reactor_ = nullptr;

  std::unique_ptr<BybitMdFeed> md_feed_;
  std::unique_ptr<BybitPrivateParser> private_parser_;
  std::unique_ptr<BybitOrderEncoder> encoder_;
  std::unique_ptr<BybitResponseDecoder> decoder_;
  std::unique_ptr<RestChannel> rest_;
  MdHandler md_handler_{this};
  PrivateHandler private_handler_{this};
  TradeHandler trade_handler_{this};
  ConnectionSlot<MdHandler> md_conn_;
  ConnectionSlot<PrivateHandler> private_conn_;
  ConnectionSlot<TradeHandler> trade_conn_;
  RateLimiter rate_;
  OpenHashMap<ClientOrderId, OrderShadow, 8192> shadows_;
  OpenHashMap<ClientOrderId, ClientOrderId, 8192> aliases_;  // venue orderLinkId -> engine id
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
  char request_buf_[kMaxRequestBytes];
  RawRecorder raw_md_;
  RawRecorder raw_private_;
  RawRecorder raw_trade_;

  std::vector<InstrumentId> subscribed_;
  std::atomic<std::int64_t> clock_offset_ms_{0};
  std::int64_t clock_sync_ns_ = 0;
  std::int64_t last_ping_ns_ = 0;
  bool time_request_pending_ = false;
  bool clock_resync_wanted_ = false;
  bool fatal_ = false;
  bool venue_kill_sent_ = false;  // TripVenueKill emitted
  bool connected_ = false;
  bool rest_hard_stopped_ = false;
  bool private_was_live_ = false;
  bool trade_was_live_ = false;
  SentWatermark sent_;
  BatchedOrders batch_;  // orders written into the corked trade connection
  // Open-order snapshot, collected across pages before anything reaches the engine.
  std::vector<ReconcileMsg> reconcile_records_;
  ClientOrderId reconcile_watermark_{};  // sent watermark when the first page was requested
  bool sweep_next_ = false;  // the next snapshot is the start-up sweep: an empty watermark
  std::size_t reconcile_pages_ = 0;
  bool reconcile_in_flight_ = false;
  bool oo_wanted_ = false;  // a snapshot waits for the execution replay in flight
  // Linear: open orders and positions are listed per settle coin (the quotes of the subscribed
  // instruments); the snapshot walks them in turn.
  std::vector<std::string> settle_coins_;
  std::size_t reconcile_coin_ = 0;
  std::vector<PositionRecord> reconcile_positions_;
  bool refused_account_settings_ = false;

  // Linear: per instrument, the position the engine holds from the fills forwarded and the last
  // one the position topic reported.
  struct PositionCheck {
    Qty tracked{};
    Qty venue{};
    Price venue_avg{};
    std::int64_t last_event_ns = 0;
    bool pending = false;  // a position-topic value has not been compared yet
  };
  std::array<PositionCheck, kMaxInstruments> positions_{};

  // Execution replay. Bybit's history has no ascending id, so the watermark is the venue time of
  // the newest execution seen (inclusive); the ids seen at exactly that time are skipped on the
  // next pass. It starts at connect(), or where resume_executions() says.
  struct ExecRow {
    InstrumentId inst;
    std::string exec_id;
    std::string order_id;
    std::string order_link_id;
    std::string price;
    std::string qty;
    std::string fee;
    std::string fee_currency;
    std::string fee_rate;
    std::int64_t time_ms = 0;
    Side side = Side::Buy;
    bool maker = false;
  };
  std::int64_t exec_since_ms_ = 0;
  std::unordered_set<std::string> exec_edge_ids_;   // ids at exec_since_ms_, already forwarded
  std::unordered_set<std::string> known_exec_ids_;  // booked by an earlier session
  std::vector<ExecRow> exec_rows_;                  // the current window, newest first as received
  std::int64_t exec_window_start_ = 0;
  std::int64_t exec_window_end_ = 0;     // 0: open-ended (the last window)
  std::int64_t exec_window_low_ms_ = 0;  // oldest row seen in the window so far
  std::size_t exec_window_pages_ = 0;
  std::size_t exec_requests_ = 0;  // pages asked for by this replay
  bool exec_replay_active_ = false;
  bool exec_replay_ok_ = true;
  bool exec_snapshot_exact_ = false;  // stamp kExecutionsExact on the next snapshot's Begin
  bool exec_retry_wanted_ = false;    // the last replay was incomplete: ask again from on_timer
  std::int64_t exec_last_ns_ = 0;     // when the last replay started (the periodic one)
  std::int64_t exec_retry_ns_ = 0;
  ConnState md_state_ = ConnState::Disconnected;
  ConnState private_state_ = ConnState::Disconnected;
  ConnState trade_state_ = ConnState::Disconnected;
  net::TimerId housekeeping_timer_ = net::kInvalidTimer;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);

  WireLatencyRecorder wire_;  // reactor thread; summarized into stats_ by publish_status()
  VenueStatus stats_{};
  Seqlocked<VenueStatus> published_{};
};

// [venues.<name>] -> BybitVenueConfig. ws_url = public stream of the category, ws_api_url = trade
// stream; extra keys: category ("spot"|"linear"), ws_private_url, depth, order_api
// ("ws"|"rest"), stale_ms, dead_ms, ping_interval_ms, position_from_wallet, position_from_stream,
// allow_offline_reference_data, cancel_on_order_channel_loss, emit_ack_from_response,
// orders_per_second, dead_mans_switch_s. Throws std::invalid_argument for an unknown category.
BybitVenueConfig make_bybit_config(const VenueSection& section, bool dry_run);

}  // namespace fastmm::venues::bybit
