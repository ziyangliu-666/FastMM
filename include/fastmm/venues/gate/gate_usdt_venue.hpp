#pragma once
// GateUsdtVenue: the Gate APIv4 connector for USDT-settled perpetual futures (BTC_USDT,
// NVDA_USDT, ...), single position mode.
//
// Channels on one reactor thread (URLs: futures WS docs "Server URL", REST docs, read 2026-10-03):
//   md       wss://fx-ws.gateio.ws/v4/ws/usdt   futures.obu (book), futures.book_ticker,
//                                               futures.trades, futures.tickers (GateMdFeed)
//   private  the same URL, a second connection  futures.login (WebSocket API), then
//                                               futures.orders / usertrades / positions /
//                                               balances subscribed with the auth block and the
//                                               account's user id
//   trade    the same URL, a third connection   futures.login, then futures.order_place /
//                                               order_cancel / order_amend; REST fallback
//   rest     https://fx-api.gateio.ws           /api/v4/futures/usdt/{contracts, accounts,
//                                               positions, orders, my_trades_timerange, fee,
//                                               countdown_cancel_all, order_book (the clock)}
// Every WebSocket connection sends futures.ping every ping_interval_ms; the upgrade carries
// "X-Gate-Size-Decimal: 1" so sizes come as strings (decimal contracts). cancel_all() uses an
// independent BlockingHttp connection (DELETE /orders?contract=X per subscribed contract).
//
// Units: contracts. Instrument::contract_multiplier = quanto_multiplier (base units per contract),
// tick = order_price_round, lot 1 contract, min_qty = order_size_min, max_qty = order_size_max;
// base = the contract's prefix (NVDA of NVDA_USDT), quote = the settle currency (USDT), which is
// what [accounting] settles it in. Only `type` direct (linear) is accepted.
//
// Account: load_reference_data() reads GET /accounts (the user id the private channels need, the
// balances) and refuses an account in dual position mode (refused_account_settings()); a
// position with mode other than single seen later is fatal for the venue.
//
// Reconciliation: the fill replay (GET /my_trades_timerange over the account, one stream, paged by
// offset, rows newest first) before GET /orders?status=open per subscribed contract and GET
// /positions?holding=true, as one Begin / OpenOrder* / Position* / End; an absent contract is flat.
// Between reconciliations the positions channel is compared, as on Bybit linear, with the
// connector's sum of the fills it forwarded, and a PositionUpdateMsg corrects the engine only when
// they differ once neither has changed for kPositionSettleMs.
//
// Balances: the driver's balance leg reads GET /accounts after every snapshot; a futures.balances
// push (which carries the balance of one change only) asks for the balances again (at most 1/s).
//
// Amend (Replace) keeps the venue's order and its text: the engine gets an ack (kAmendedInPlace)
// for its new client id and later orders/usertrades events for the original text are translated
// to it.
#include "fastmm/config/config.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/net/connection.hpp"
#include "fastmm/venues/connection_slot.hpp"
#include "fastmm/venues/dead_mans_switch.hpp"
#include "fastmm/venues/gate/gate_auth.hpp"
#include "fastmm/venues/gate/gate_error_map.hpp"
#include "fastmm/venues/gate/gate_md_feed.hpp"
#include "fastmm/venues/gate/gate_order_encoder.hpp"
#include "fastmm/venues/gate/gate_private_parser.hpp"
#include "fastmm/venues/gate/gate_rest_decoder.hpp"
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
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::gate {

struct GateUsdtVenueConfig {
  std::string name = "gate_usdt";
  std::string ws_url;    // wss://fx-ws.gateio.ws/v4/ws/usdt
  std::string rest_url;  // https://fx-api.gateio.ws
  std::string settle = "usdt";
  Credentials credentials;
  bool insecure_tls = false;
  std::string ca_file;
  // [venues.<name>] source_ip / source_interface: every connection goes out from it.
  net::SourceAddress source;
  bool dry_run = false;
  bool ws_order_api = true;
  bool emit_ack_from_response = true;
  bool position_from_stream = true;
  bool cancel_on_order_channel_loss = true;
  bool allow_offline_reference_data = false;
  bool fetch_fees = false;
  bool supports_replace = false;
  int book_level = 50;  // futures.obu depth: 50 (20 ms) | 400 (100 ms)
  std::uint32_t stale_ms = 2000;
  std::uint32_t dead_ms = 30'000;
  std::uint32_t ping_interval_ms = 15'000;
  // Client-side cap on new orders and amends per second (the venue: 100 r/s per UID for
  // placement and amend, 200 r/s for cancels).
  std::uint32_t orders_per_second = 50;
  double rate_threshold = 0.9;
  // POST /countdown_cancel_all window in seconds; 0 disables the dead man's switch.
  std::int64_t dead_mans_switch_s = 0;
  std::string record_raw_dir;
  std::uint32_t http_timeout_ms = 5000;
  net::BackoffConfig backoff{};
};

class GateUsdtVenue final : public Venue, private ReconcileHooks {
 public:
  GateUsdtVenue(VenueId id, GateUsdtVenueConfig cfg);
  ~GateUsdtVenue() override;

  [[nodiscard]] VenueId id() const noexcept override { return id_; }
  [[nodiscard]] std::string_view name() const noexcept override { return cfg_.name; }
  [[nodiscard]] VenueCaps caps() const noexcept override;
  Result<void, std::string> load_reference_data(InstrumentTable& instruments) override;
  Result<std::vector<VenueFee>, std::string> account_fees(
      const InstrumentTable& instruments) override;
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

  [[nodiscard]] const GateMdFeed* md_feed() const noexcept { return md_feed_.get(); }
  [[nodiscard]] const GateUsdtVenueConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] bool fatal() const noexcept { return fatal_; }
  [[nodiscard]] std::size_t shadow_count() const noexcept { return shadows_.size(); }
  [[nodiscard]] bool order_channel_live() const noexcept { return trade_conn_.is_live(); }
  [[nodiscard]] std::int64_t clock_offset_ms() const noexcept { return clock_offset_ms_.load(); }
  [[nodiscard]] std::int64_t venue_time_ms() const noexcept;
  [[nodiscard]] std::int64_t venue_time_s() const noexcept { return venue_time_ms() / 1000; }
  [[nodiscard]] const std::string& user_id() const noexcept { return user_id_; }

 private:
  struct MdHandler {
    GateUsdtVenue* v;
    void on_state(net::ConnState s) { v->on_md_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_md_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_md_open(); }
  };
  struct PrivateHandler {
    GateUsdtVenue* v;
    void on_state(net::ConnState s) { v->on_private_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_private_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_private_open(); }
  };
  struct TradeHandler {
    GateUsdtVenue* v;
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
  net::ConnectionConfig ws_config(bool manual_auth, bool manual_subscribe) const;
  // futures.login on the private or the trade connection (req_id "login-p" / "login-t").
  void send_login(bool trade);
  void drain_outbound();
  template <class Ring>
  void write_orders(Ring& ring);
  void send_command(const OrderCommand& cmd);
  void send_command_rest(const OrderCommand& cmd, const OrderShadow* shadow);
  void fail_batch();
  void handle_order_response(RequestKind kind, ClientOrderId id, const ApiResponse& r);
  void handle_rest_order_response(const OrderCommand& cmd, const net::HttpResponse& r);
  void trip_venue_kill(KillReason reason);
  void apply_action(VenueAction action,
                    std::string_view label,
                    std::string_view msg,
                    std::int64_t reset_epoch_ms);
  void request_resubscribe(InstrumentId id);
  void request_server_time();
  void cancel_all_async();
  // Reconciliation: GET /orders?status=open per subscribed contract (paged), then positions.
  bool request_open_orders_page(std::uint64_t generation, std::size_t index, int offset);
  bool request_positions(std::uint64_t generation);
  void finish_snapshot(std::uint64_t generation);
  // Start-up: GET /accounts (user id, dual mode, balances).
  std::string check_account(AccountInfo& out);
  void check_positions(std::int64_t now);
  void note_fill(const OrderFillMsg& f) noexcept;
  // ReconcileHooks
  bool fetch_snapshot(std::uint64_t generation) override;
  bool fetch_balances(std::uint64_t generation) override;
  bool replay_executions() override;
  void shadow_ids(std::vector<SentShadow>& out) override;
  void drop_shadow(ClientOrderId id) override;
  // Fill replay (ReplayScheduler): GET /my_trades_timerange, newest first, paged by offset.
  struct TradeRow;
  [[nodiscard]] bool replay_ready() const noexcept;
  bool query_trades(const ReplayQuery& q);
  bool emit_trade(const TradeRow& t);
  // Dead man's switch: POST /countdown_cancel_all for the account.
  void send_countdown(std::int64_t timeout_s);
  void stop_countdown_blocking();
  void publish_status() noexcept;
  void refuse_untracked(const OrderCommand& cmd);
  void note_rate_headers(const net::HttpResponse& r);
  [[nodiscard]] static std::int64_t reset_ms_of(std::int64_t v) noexcept;
  void forget_order(ClientOrderId id) noexcept;
  [[nodiscard]] ClientOrderId current_id(ClientOrderId link) const noexcept;
  // A fill's cumulative and remaining quantity from the order's shadow.
  void complete_fill(OrderFillMsg& f) noexcept;
  [[nodiscard]] std::int64_t now_ns() const noexcept { return net::Reactor::now_ns(); }
  static void resubscribe_requester(void* ctx, InstrumentId id) noexcept {
    static_cast<GateUsdtVenue*>(ctx)->request_resubscribe(id);
  }

  VenueId id_;
  GateUsdtVenueConfig cfg_;
  Signer signer_;
  const SymbolTable* symbols_ = nullptr;
  const InstrumentTable* instruments_ = nullptr;
  EventSink* md_sink_ = nullptr;
  EventSink* order_sink_ = nullptr;
  MsgRing* outbound_ = nullptr;
  net::Reactor* reactor_ = nullptr;

  std::unique_ptr<GateMdFeed> md_feed_;
  std::unique_ptr<GatePrivateParser> private_parser_;
  std::unique_ptr<GateOrderEncoder> encoder_;
  std::unique_ptr<GateResponseDecoder> decoder_;
  std::unique_ptr<RestChannel> rest_;
  MdHandler md_handler_{this};
  PrivateHandler private_handler_{this};
  TradeHandler trade_handler_{this};
  ConnectionSlot<MdHandler> md_conn_;
  ConnectionSlot<PrivateHandler> private_conn_;
  ConnectionSlot<TradeHandler> trade_conn_;
  RateLimiter rate_;
  OpenHashMap<ClientOrderId, OrderShadow, kShadowSlots> shadows_;
  ShadowOverflow shadow_overflow_;
  OpenHashMap<ClientOrderId, ClientOrderId, kShadowSlots> aliases_;  // venue text id -> engine id
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
  char request_buf_[kMaxRequestBytes];
  RawRecorder raw_md_;
  RawRecorder raw_private_;
  RawRecorder raw_trade_;

  std::vector<InstrumentId> subscribed_;
  // Reference data for the tickers channel, per instrument (contract table).
  std::array<Duration, kMaxInstruments> funding_interval_{};
  std::array<std::int64_t, kMaxInstruments> funding_next_ms_{};
  // The private channels subscribed after the login: orders, usertrades, positions, balances.
  static constexpr std::uint32_t kPrivateChannels = 4;
  std::uint32_t private_subscribed_ = 0;  // acks received on this connection
  std::string user_id_;  // the account's id (GET /accounts `user`, or the login reply's uid)
  std::atomic<std::int64_t> clock_offset_ms_{0};
  std::int64_t clock_sync_ns_ = 0;
  std::int64_t last_ping_ns_ = 0;
  bool time_request_pending_ = false;
  bool clock_resync_wanted_ = false;
  bool fatal_ = false;
  bool venue_kill_sent_ = false;
  bool connected_ = false;
  bool rest_hard_stopped_ = false;
  bool private_was_live_ = false;
  bool trade_was_live_ = false;
  bool refused_account_settings_ = false;
  SentWatermark sent_;
  BatchedOrders batch_;
  ReconcileDriver reconcile_{*this, sent_};
  std::vector<ReconcileMsg> reconcile_orders_;  // rows read so far, emitted at finish_snapshot
  std::vector<PositionRecord> reconcile_positions_;
  std::size_t reconcile_pages_ = 0;
  CountdownDriver dms_;

  struct PositionCheck {
    Qty tracked{};
    Qty venue{};
    Price venue_avg{};
    std::int64_t last_event_ns = 0;
    bool pending = false;
  };
  std::array<PositionCheck, kMaxInstruments> positions_{};

  // A row of GET /my_trades_timerange.
  struct TradeRow {
    InstrumentId inst;
    std::string id;
    std::string order_id;
    std::string text;
    Qty size{};  // signed
    Price price{};
    Notional fee{};
    bool maker = false;
    std::int64_t time_ms = 0;
  };
  ReplayScheduler<TradeRow> trade_replay_;
  ConnState md_state_ = ConnState::Disconnected;
  ConnState private_state_ = ConnState::Disconnected;
  ConnState trade_state_ = ConnState::Disconnected;
  net::TimerId housekeeping_timer_ = net::kInvalidTimer;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);

  WireLatencyRecorder wire_;
  VenueStatus stats_{};
  Seqlocked<VenueStatus> published_{};
};

// [venues.<name>] -> GateUsdtVenueConfig. ws_url = the futures stream, rest_url = the API host;
// extra keys: settle, order_api ("ws"|"rest"), book_level (50|400), stale_ms, dead_ms,
// ping_interval_ms, orders_per_second, emit_ack_from_response, position_from_stream,
// cancel_on_order_channel_loss, allow_offline_reference_data, fetch_fees, dead_mans_switch_s.
GateUsdtVenueConfig make_gate_usdt_config(const VenueSection& section, bool dry_run);

}  // namespace fastmm::venues::gate
