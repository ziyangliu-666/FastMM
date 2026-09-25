#pragma once
// DeribitVenue: the Deribit options/futures connector (JSON-RPC 2.0 over WebSocket).
//
// Channels on one reactor thread (endpoints: https://docs.deribit.com/articles/json-rpc-overview):
//   md       ws_url (wss://test.deribit.com/ws/api/v2), unauthenticated: public/set_heartbeat, then
//            public/subscribe book/ticker/trades per instrument (DeribitMdFeed)
//   private  ws_private_url (default: ws_url), a second connection: public/auth
//            (client_credentials), then public/set_heartbeat, private/enable_cancel_on_disconnect
//            (optional), private/subscribe user.orders.KIND.CURRENCY.raw and
//            user.trades.KIND.CURRENCY.raw; order entry (private/buy, sell, edit, cancel,
//            cancel_by_label) and reconciliation (private/get_user_trades_by_currency_and_time,
//            then private/get_open_orders_by_currency) run here too
//   rest     rest_url (https://test.deribit.com/api/v2): public/get_instruments and public/get_time
//            at startup; private/cancel_all_by_instrument with HTTP Basic credentials for the
//            kill switch (independent BlockingHttp) and after the private channel drops
//
// Heartbeats: both connections call public/set_heartbeat (interval >= 10 s) and answer every
// {"method":"heartbeat","params":{"type":"test_request"}} with public/test; the server closes a
// connection that does not ("Heartbeats", connection-management article; recorded on testnet).
//
// Tokens: public/auth returns access_token, refresh_token and expires_in (seconds). The token is
// refreshed with grant_type=refresh_token at token_refresh_fraction of its lifetime; an order
// answered with 13009 unauthorized re-authenticates with the client credentials.
//
// Edits keep the venue's label (the first client id): the engine gets an ack for its new client
// id and later user.orders / user.trades events for the old label are translated to it.
//
// Rate limits: matching-engine requests (buy/sell/edit) are refused locally when the
// CreditBucket (matching_engine_rate / _burst, Tier 4 defaults) is empty; cancels are always
// sent. A 10028 too_many_requests drains the bucket (the venue also drops the session).
#include "fastmm/config/config.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/net/connection.hpp"
#include "fastmm/venues/connection_slot.hpp"
#include "fastmm/venues/deribit/deribit_credits.hpp"
#include "fastmm/venues/deribit/deribit_error_map.hpp"
#include "fastmm/venues/deribit/deribit_md_feed.hpp"
#include "fastmm/venues/deribit/deribit_order_encoder.hpp"
#include "fastmm/venues/deribit/deribit_private_parser.hpp"
#include "fastmm/venues/order_commands.hpp"
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

namespace fastmm::venues::deribit {

struct DeribitVenueConfig {
  std::string name = "deribit";
  std::string ws_url;          // wss://test.deribit.com/ws/api/v2
  std::string ws_private_url;  // defaults to ws_url
  std::string rest_url;        // https://test.deribit.com/api/v2
  Credentials credentials;
  bool insecure_tls = false;
  std::string ca_file;
  bool dry_run = false;
  bool supports_replace = true;  // private/edit
  std::vector<std::string> currencies{"BTC"};
  MdIntervals intervals{};
  std::int64_t heartbeat_interval_s = 10;
  std::uint32_t stale_ms = 10'000;
  std::uint32_t dead_ms = 30'000;
  bool reject_post_only = true;
  bool cancel_on_disconnect = true;
  bool cancel_on_order_channel_loss = true;
  bool allow_offline_reference_data = false;
  bool emit_ack_from_response = true;
  std::int64_t matching_engine_rate = 5;
  std::int64_t matching_engine_burst = 20;
  double token_refresh_fraction = 0.8;
  std::string record_raw_dir;
  std::uint32_t http_timeout_ms = 5000;
  net::BackoffConfig backoff{};
};

// JSON-RPC ids of control requests (order requests use string ids, see request_id.hpp).
inline constexpr std::int64_t kIdAuth = 1;
inline constexpr std::int64_t kIdRefresh = 2;
inline constexpr std::int64_t kIdHeartbeat = 3;
inline constexpr std::int64_t kIdTest = 4;
inline constexpr std::int64_t kIdCancelOnDisconnect = 5;
inline constexpr std::int64_t kIdPrivateSubscribe = 6;
inline constexpr std::int64_t kIdReauth = 7;
inline constexpr std::int64_t kIdOpenOrdersBase = 100;   // + currency index
inline constexpr std::int64_t kIdExecutionsBase = 1000;  // + currency index

class DeribitVenue final : public Venue {
 public:
  DeribitVenue(VenueId id, DeribitVenueConfig cfg);
  ~DeribitVenue() override;

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
  void on_wake() override;
  void send_now(std::span<const EventHeader* const> batch) override;
  void request_open_orders() override;
  bool request_executions(std::int64_t since_venue_ms = 0) override;
  void resume_executions(std::int64_t since_venue_ms,
                         const std::vector<std::string>& known) override;
  bool cancel_all() override;
  [[nodiscard]] VenueStatus status() const noexcept override;

  [[nodiscard]] const DeribitMdFeed* md_feed() const noexcept { return md_feed_.get(); }
  [[nodiscard]] const DeribitVenueConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] bool fatal() const noexcept { return fatal_; }
  [[nodiscard]] bool authenticated() const noexcept { return !access_token_.empty(); }
  [[nodiscard]] const TickSchedule& tick_schedule(InstrumentId id) const noexcept {
    return ticks_[id.value < kMaxInstruments ? id.value : 0];
  }
  [[nodiscard]] std::vector<std::string> private_channels() const;
  [[nodiscard]] std::int64_t clock_offset_ms() const noexcept { return clock_offset_ms_.load(); }

 private:
  struct MdHandler {
    DeribitVenue* v;
    void on_state(net::ConnState s) { v->on_md_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_md_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_md_open(); }
  };
  struct PrivateHandler {
    DeribitVenue* v;
    void on_state(net::ConnState s) { v->on_private_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_private_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_private_open(); }
  };

  void on_md_state(net::ConnState s);
  void on_md_text(std::string_view t, std::int64_t ts);
  void on_md_open();
  void on_private_state(net::ConnState s);
  void on_private_text(std::string_view t, std::int64_t ts);
  void on_private_open();

  void open_md();
  void open_private();
  [[nodiscard]] net::ConnectionConfig ws_config(const std::string& url, bool authenticated) const;
  void send_auth(std::int64_t id);
  void send_private(std::string_view frame, std::string_view what);
  void handle_control_response(const PrivateDecodeResult& r);
  void handle_order_response(RequestKind kind, ClientOrderId id, const PrivateDecodeResult& r);
  void handle_open_orders_response(std::size_t currency_index, std::string_view json, bool error);
  // The open-order snapshot request itself, once any execution replay before it has finished.
  void send_open_orders();
  // Execution replay (see deribit_venue.cpp): one query per currency at a time, paged.
  bool send_executions_query(std::size_t currency_index);
  void handle_executions_response(std::size_t currency_index, std::string_view json, bool error);
  void finish_executions_for(std::size_t currency_index, bool ok);
  [[nodiscard]] std::int64_t venue_now_ms() const noexcept;
  void drain_outbound();
  // Encodes and writes the orders `ring` holds (the outbound MsgRing or an OutboundBatch).
  template <class Ring>
  void write_orders(Ring& ring);
  void send_command(const OrderCommand& cmd);
  // uncork() failed: the batch never left, so its orders are rejected (see BatchedOrders).
  void fail_batch();
  // Sends ControlCommand::TripVenueKill to the engine, once per session.
  void trip_venue_kill(KillReason reason);
  void apply_action(VenueAction action, int code, std::string_view msg);
  void request_resubscribe(InstrumentId id);
  void cancel_all_async();
  void publish_status() noexcept;
  void forget_order(ClientOrderId id) noexcept;
  [[nodiscard]] ClientOrderId current_id(ClientOrderId label) const noexcept;
  [[nodiscard]] std::int64_t now_ns() const noexcept { return net::Reactor::now_ns(); }
  static void resubscribe_requester(void* ctx, InstrumentId id) noexcept {
    static_cast<DeribitVenue*>(ctx)->request_resubscribe(id);
  }

  VenueId id_;
  DeribitVenueConfig cfg_;
  const SymbolTable* symbols_ = nullptr;
  const InstrumentTable* instruments_ = nullptr;
  EventSink* md_sink_ = nullptr;
  EventSink* order_sink_ = nullptr;
  MsgRing* outbound_ = nullptr;
  net::Reactor* reactor_ = nullptr;

  std::array<TickSchedule, kMaxInstruments> ticks_{};
  std::unique_ptr<DeribitMdFeed> md_feed_;
  std::unique_ptr<DeribitPrivateParser> private_parser_;
  std::unique_ptr<DeribitOrderEncoder> encoder_;
  std::unique_ptr<RestChannel> rest_;
  MdHandler md_handler_{this};
  PrivateHandler private_handler_{this};
  ConnectionSlot<MdHandler> md_conn_;
  ConnectionSlot<PrivateHandler> private_conn_;
  CreditBucket matching_credits_;
  OpenHashMap<ClientOrderId, OrderShadow, 8192> shadows_;
  OpenHashMap<ClientOrderId, ClientOrderId, 8192> aliases_;  // venue label -> engine id
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
  char request_buf_[kMaxRequestBytes];
  RawRecorder raw_md_;
  RawRecorder raw_private_;

  std::vector<InstrumentId> subscribed_;
  std::string access_token_;
  std::string refresh_token_;
  std::int64_t refresh_at_ns_ = 0;
  std::uint32_t private_channels_requested_ = 0;
  bool user_channels_ok_ = false;
  bool auth_in_flight_ = false;
  // Reconciliation across currencies: records are collected until every response arrived.
  std::vector<ReconcileMsg> reconcile_records_;
  std::size_t reconcile_pending_ = 0;
  bool reconcile_failed_ = false;
  ClientOrderId reconcile_watermark_{};  // sent watermark when the open orders were requested
  bool sweep_next_ = false;  // the next snapshot is the start-up sweep: an empty watermark
  bool oo_wanted_ = false;   // a snapshot waits for the execution replay in flight

  // Execution replay, per currency: the next query starts at since_ms (inclusive, the timestamp
  // of the last row forwarded) and skips edge_ids, the rows at since_ms already forwarded.
  struct ExecCursor {
    std::int64_t since_ms = 0;
    std::vector<std::string> edge_ids;
    std::int64_t query_start_ms = 0;  // start_timestamp of the query in flight
    std::uint32_t pages = 0;          // pages fetched in this replay
    bool historical = false;          // the query in flight has historical: true
  };
  std::vector<ExecCursor> exec_cursors_;  // parallel to cfg_.currencies
  std::int64_t exec_since_ms_ = 0;        // where a cursor starts: connect() or resume_executions()
  std::int64_t exec_now_ms_ = 0;          // venue time the replay in flight started at
  // Trade ids an earlier session booked; a resumed replay skips them (resume_executions).
  std::unordered_set<std::string> known_exec_ids_;
  // Trade ids forwarded by the replay in flight: the historical and recent queries overlap.
  std::unordered_set<std::string> exec_seen_;
  std::size_t exec_pending_ = 0;  // currencies still being fetched
  bool exec_replay_ok_ = true;    // every currency so far was fetched in full
  bool exec_replay_active_ = false;
  bool exec_snapshot_exact_ = false;  // stamp kExecutionsExact on the next snapshot's Begin
  bool exec_retry_wanted_ = false;    // the last replay was incomplete: ask again from on_timer
  std::int64_t exec_retry_ns_ = 0;
  SentWatermark sent_;
  BatchedOrders batch_;  // orders written into the corked private connection

  std::atomic<std::int64_t> clock_offset_ms_{0};
  bool fatal_ = false;
  bool venue_kill_sent_ = false;  // TripVenueKill emitted
  bool connected_ = false;
  bool private_was_live_ = false;
  ConnState md_state_ = ConnState::Disconnected;
  ConnState private_state_ = ConnState::Disconnected;
  net::TimerId housekeeping_timer_ = net::kInvalidTimer;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);

  WireLatencyRecorder wire_;
  VenueStatus stats_{};
  Seqlocked<VenueStatus> published_{};
};

// [venues.<name>] -> DeribitVenueConfig. api_key / api_secret are the client id and client secret.
// Extra keys: ws_private_url, currencies ("BTC" or "BTC,ETH" or a TOML array), book_interval,
// ticker_interval, trades_interval, heartbeat_interval_s, stale_ms, dead_ms, reject_post_only,
// cancel_on_disconnect, cancel_on_order_channel_loss, allow_offline_reference_data,
// emit_ack_from_response, matching_engine_rate, matching_engine_burst.
DeribitVenueConfig make_deribit_config(const VenueSection& v, bool dry_run);

// public/get_instruments entry -> Instrument fields and the price grid (empty string on success):
//   kind option -> AssetClass::Option (option_type, strike, expiry); future with settlement_period
//   perpetual -> Perpetual, other futures -> Future (expiry); spot -> Spot
//   tick = tick_size, tick_size_steps -> TickSchedule; contract_multiplier = contract_size;
//   lot = min_qty = min_trade_amount / contract_size (contracts); kInverse for instrument_type
//   reversed; base/quote = base_currency/quote_currency; disabled unless is_active and state open.
std::string apply_instrument_info(const InstrumentInfo& f, Instrument& inst, TickSchedule& ticks);

}  // namespace fastmm::venues::deribit
