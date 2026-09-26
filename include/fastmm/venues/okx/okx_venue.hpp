#pragma once
// OkxVenue: the OKX v5 connector for USDT-margined perpetual swaps (instType SWAP, ctType
// linear, e.g. BTC-USDT-SWAP), net position mode only.
//
// Channels on one reactor thread (https://www.okx.com/docs-v5/en/#overview-production-trading-
// services and #overview-demo-trading-services, read 2026-09-26):
//   md       wss://ws.okx.com:8443/ws/v5/public     books (or books50-l2-tbt / books-l2-tbt, which
//            (demo wss://wspap.okx.com:8443/...)     need VIP4 and a login) / bbo-tbt / trades
//   private  wss://ws.okx.com:8443/ws/v5/private    op login, then orders, positions and
//                                                   balance_and_position (instType SWAP)
//   trade    wss://ws.okx.com:8443/ws/v5/private    op login, then order / amend-order /
//            (a second connection)                  cancel-order; REST fallback
//   rest     https://www.okx.com                    public/instruments, public/time, account/
//                                                   config, trade/orders-pending, trade/fills[-
//                                                   history], account/positions, account/bills[-
//                                                   archive], trade/cancel-batch-orders, trade/
//                                                   cancel-all-after, REST order entry
// Demo trading uses the same REST host with the header "x-simulated-trading: 1", sent when the
// section's `testnet` is true (the default). Every WebSocket connection sends the text "ping"
// every ping_interval_ms: OKX closes one that has had no data for 30 s. cancel_all() uses an
// independent BlockingHttp connection.
//
// Units. OKX sizes a swap in contracts of ctVal (times ctMult) of ctValCcy, the base coin: 0.01 BTC
// for BTC-USDT-SWAP. FastMM's quantity for the instrument is contracts too, everywhere: book and
// trade sizes, order sz, fills, positions. Instrument::contract_multiplier = ctVal * ctMult, so
// notional = px * qty * multiplier in the settlement currency (USDT) and PnL is linear in it;
// base = ctValCcy, quote = settleCcy, which is what [accounting] settles it in. tick = tickSz,
// lot = lotSz, min_qty = minSz, max_qty = maxLmtSz; OKX has no minimum notional for swaps.
//
// Position mode. With keys, load_reference_data() reads GET /api/v5/account/config and refuses
// long/short mode (posMode long_short_mode) and the spot account mode (acctLv 1, which cannot
// trade swaps): refused_account_settings(), fastmm-live exits 3. A long/short position seen later
// (positions channel or reconciliation) is fatal for the venue. tdMode is the `td_mode` key,
// cross by default; leverage and margin are account settings the connector does not touch.
//
// Reconciliation: the fills since the watermark (GET /api/v5/trade/fills, fills-history beyond 3
// days) are replayed as fills with the tradeId as execution id; then orders-pending and
// account/positions become one Begin / OpenOrder* / Position* / End, a subscribed instrument absent
// from the positions being flat. Funding: GET /api/v5/account/bills type 8 (funding fee), one
// FundingMsg per bill (billId, balChg in ccy), with every execution replay and a second after a
// balance_and_position push with eventType funding_fee.
//
// Amend (Replace) keeps the venue's clOrdId and names the engine's new id in reqId: the orders
// channel reports the result (amendResult) under it, which becomes the ack or reject of the new id;
// later events for the original clOrdId are translated to it.
//
// Dead man's switch: POST /api/v5/trade/cancel-all-after {"timeOut":"<s>"}, a countdown for every
// pending order of the account, refreshed from the housekeeping timer every window/3 (the same
// shape as Binance USD-M countdownCancelAll); timeOut "0" on disconnect().
#include "fastmm/config/config.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/net/connection.hpp"
#include "fastmm/venues/connection_slot.hpp"
#include "fastmm/venues/dead_mans_switch.hpp"
#include "fastmm/venues/okx/okx_auth.hpp"
#include "fastmm/venues/okx/okx_error_map.hpp"
#include "fastmm/venues/okx/okx_md_feed.hpp"
#include "fastmm/venues/okx/okx_order_encoder.hpp"
#include "fastmm/venues/okx/okx_private_parser.hpp"
#include "fastmm/venues/okx/okx_rest_decoder.hpp"
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

namespace fastmm::venues::okx {

struct OkxVenueConfig {
  std::string name = "okx";
  std::string ws_public_url;   // wss://ws.okx.com:8443/ws/v5/public
  std::string ws_private_url;  // wss://ws.okx.com:8443/ws/v5/private
  std::string ws_trade_url;    // order operations; the private URL by default
  std::string rest_url;        // https://www.okx.com
  Credentials credentials;
  bool simulated = true;  // demo trading: x-simulated-trading: 1 on every REST request
  TdMode td_mode = TdMode::Cross;
  OkxDepthChannel depth_channel = OkxDepthChannel::Books;
  bool insecure_tls = false;
  std::string ca_file;
  bool dry_run = false;
  bool ws_order_api = true;
  bool emit_ack_from_response = true;
  bool position_from_stream = true;  // correct the engine from the positions channel
  bool cancel_on_order_channel_loss = true;
  // cancel-all-after window, seconds: 0 disables it, otherwise 10 to 120. On by default: it is
  // what clears the quotes after a SIGKILL, an OOM kill or a dead host.
  int dead_mans_switch_s = 60;
  bool allow_offline_reference_data = false;
  bool supports_replace = true;
  std::uint32_t stale_ms = 2000;
  std::uint32_t dead_ms = 30'000;
  std::uint32_t ping_interval_ms = 20'000;  // below OKX's 30 s idle limit
  // Place and amend: 60 requests per 2 s per instrument (User ID + Instrument ID), and 1000 per
  // 2 s for the sub-account. 25/s keeps one instrument under its limit with room to spare.
  std::uint32_t orders_per_second = 25;
  double rate_threshold = 0.9;
  std::string record_raw_dir;
  std::uint32_t http_timeout_ms = 5000;
  net::BackoffConfig backoff{};
};

class OkxVenue final : public Venue {
 public:
  OkxVenue(VenueId id, OkxVenueConfig cfg);
  ~OkxVenue() override;

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

  [[nodiscard]] const OkxMdFeed* md_feed() const noexcept { return md_feed_.get(); }
  [[nodiscard]] const OkxVenueConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] bool fatal() const noexcept { return fatal_; }
  // The order connection is logged in (reactor thread; tests).
  [[nodiscard]] bool order_channel_live() const noexcept { return trade_conn_.is_live(); }
  [[nodiscard]] std::int64_t clock_offset_ms() const noexcept { return clock_offset_ms_.load(); }
  [[nodiscard]] std::int64_t venue_time_ms() const noexcept;
  [[nodiscard]] std::int64_t inst_id_code(InstrumentId id) const noexcept {
    return id.value < inst_codes_.size() ? inst_codes_[id.value] : -1;
  }

 private:
  struct MdHandler {
    OkxVenue* v;
    void on_state(net::ConnState s) { v->on_md_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_md_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_md_open(); }
  };
  struct PrivateHandler {
    OkxVenue* v;
    void on_state(net::ConnState s) { v->on_private_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_private_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_private_open(); }
  };
  struct TradeHandler {
    OkxVenue* v;
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
  [[nodiscard]] bool md_needs_login() const noexcept;
  [[nodiscard]] net::ConnectionConfig ws_config(const std::string& url,
                                                bool manual_auth,
                                                bool manual_subscribe) const;
  template <class Slot>
  void send_login(Slot& slot);
  [[nodiscard]] std::string rest_headers(const RestRequest& rr) const;
  void drain_outbound();
  template <class Ring>
  void write_orders(Ring& ring);
  void send_command(const OrderCommand& cmd);
  void send_command_rest(const OrderCommand& cmd, const OrderShadow* shadow);
  void fail_batch();
  void handle_order_response(RequestKind kind, ClientOrderId id, const TradeResponse& r);
  void handle_rest_order_response(const OrderCommand& cmd, const net::HttpResponse& r);
  void on_amend_result(const OrderAckMsg& ack) noexcept;
  void trip_venue_kill(KillReason reason);
  void apply_action(VenueAction action, int code, std::string_view msg);
  void request_resubscribe(InstrumentId id);
  void request_server_time();
  // Order-channel loss: orders-pending, then cancel-batch-orders, asynchronously.
  void cancel_all_async();
  void cancel_pending_async(const std::string& after, std::size_t pages);
  void send_cancel_all_after(int timeout_s);
  // disconnect(): timeOut "0" over an independent blocking connection.
  void stop_cancel_all_after();
  // One reconciliation: orders-pending (paged), then positions, then the snapshot.
  void send_open_orders();
  void request_open_orders_page(const std::string& after);
  void request_positions();
  void emit_reconcile();
  // Start-up: GET /api/v5/account/config; an error when the account cannot trade here.
  std::string check_account();
  void check_positions(std::int64_t now);
  void note_fill(const OrderFillMsg& f) noexcept;
  // Execution replay (fills / fills-history): one window from the watermark, paged newest first
  // with `after` = billId; emitted oldest first once the window's last page is in.
  void start_execution_window();
  void request_executions_page(const std::string& after);
  void on_executions_window_done();
  void emit_executions();
  void finish_execution_replay(bool ok);
  // Funding replay (bills type 8), from its own watermark.
  void request_funding(const std::string& after);
  void emit_funding_rows();
  void publish_status() noexcept;
  void forget_order(ClientOrderId id) noexcept;
  [[nodiscard]] ClientOrderId current_id(ClientOrderId link) const noexcept;
  [[nodiscard]] InstrumentId subscribed_instrument(std::string_view inst_id) const noexcept;
  [[nodiscard]] std::int64_t now_ns() const noexcept { return net::Reactor::now_ns(); }
  static void resubscribe_requester(void* ctx, InstrumentId id) noexcept {
    static_cast<OkxVenue*>(ctx)->request_resubscribe(id);
  }

  VenueId id_;
  OkxVenueConfig cfg_;
  Signer signer_;
  const SymbolTable* symbols_ = nullptr;
  const InstrumentTable* instruments_ = nullptr;
  EventSink* md_sink_ = nullptr;
  EventSink* order_sink_ = nullptr;
  MsgRing* outbound_ = nullptr;
  net::Reactor* reactor_ = nullptr;
  std::array<std::int64_t, kMaxInstruments> inst_codes_{};  // instIdCode, -1 unknown

  std::unique_ptr<OkxMdFeed> md_feed_;
  std::unique_ptr<OkxPrivateParser> private_parser_;
  std::unique_ptr<OkxOrderEncoder> encoder_;
  std::unique_ptr<OkxResponseDecoder> decoder_;
  std::unique_ptr<RestChannel> rest_;
  MdHandler md_handler_{this};
  PrivateHandler private_handler_{this};
  TradeHandler trade_handler_{this};
  ConnectionSlot<MdHandler> md_conn_;
  ConnectionSlot<PrivateHandler> private_conn_;
  ConnectionSlot<TradeHandler> trade_conn_;
  RateLimiter rate_;
  CountdownSwitch dms_;
  OpenHashMap<ClientOrderId, OrderShadow, 8192> shadows_;
  OpenHashMap<ClientOrderId, ClientOrderId, 8192> aliases_;  // venue clOrdId -> engine id
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
  bool venue_kill_sent_ = false;
  bool connected_ = false;
  bool rest_hard_stopped_ = false;
  bool private_was_live_ = false;
  bool trade_was_live_ = false;
  bool refused_account_settings_ = false;
  SentWatermark sent_;
  BatchedOrders batch_;

  // Open-order snapshot, collected across pages before anything reaches the engine.
  std::vector<ReconcileMsg> reconcile_records_;
  std::vector<PositionRecord> reconcile_positions_;
  ClientOrderId reconcile_watermark_{};
  bool sweep_next_ = false;  // the next snapshot is the start-up sweep: an empty watermark
  std::size_t reconcile_pages_ = 0;
  bool reconcile_in_flight_ = false;
  bool oo_wanted_ = false;  // a snapshot waits for the execution replay in flight

  // Per instrument: the position the engine holds from the fills forwarded and the last one the
  // positions channel reported.
  struct PositionCheck {
    Qty tracked{};
    Qty venue{};
    Price venue_avg{};
    std::int64_t last_event_ns = 0;
    bool pending = false;
  };
  std::array<PositionCheck, kMaxInstruments> positions_{};

  // Execution replay. The watermark is the `ts` of the newest fill forwarded (inclusive); the
  // tradeIds forwarded at exactly that time are skipped on the next pass.
  std::int64_t exec_since_ms_ = 0;
  std::unordered_set<std::string> exec_edge_ids_;
  std::unordered_set<std::string> known_exec_ids_;  // booked by an earlier session
  std::vector<FillRecord> exec_rows_;               // the current window, newest first
  std::int64_t exec_window_start_ = 0;
  std::int64_t exec_window_end_ = 0;     // 0: open-ended
  std::int64_t exec_window_low_ms_ = 0;  // oldest `ts` seen in the window so far
  std::size_t exec_window_pages_ = 0;
  std::size_t exec_requests_ = 0;
  bool exec_history_ = false;  // this replay reads fills-history
  bool exec_replay_active_ = false;
  bool exec_replay_ok_ = true;
  bool exec_snapshot_exact_ = false;
  bool exec_retry_wanted_ = false;
  std::int64_t exec_last_ns_ = 0;
  std::int64_t exec_retry_ns_ = 0;
  // Funding replay: watermark (`ts`, inclusive), the billIds forwarded at it, a query a
  // balance_and_position funding push asked for (reactor time, 0 none).
  std::int64_t funding_since_ms_ = 0;
  std::unordered_set<std::string> funding_edge_ids_;
  std::vector<BillRecord> funding_rows_;
  std::size_t funding_pages_ = 0;
  bool funding_archive_ = false;
  bool funding_active_ = false;
  bool funding_retry_wanted_ = false;
  std::int64_t funding_retry_ns_ = 0;
  std::int64_t funding_due_ns_ = 0;
  ConnState md_state_ = ConnState::Disconnected;
  ConnState private_state_ = ConnState::Disconnected;
  ConnState trade_state_ = ConnState::Disconnected;
  net::TimerId housekeeping_timer_ = net::kInvalidTimer;
  std::uint64_t generation_ = 0;  // replies of requests a disconnect() abandoned are ignored
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);

  WireLatencyRecorder wire_;
  VenueStatus stats_{};
  Seqlocked<VenueStatus> published_{};
};

// [venues.<name>] -> OkxVenueConfig. ws_url = public stream, ws_api_url = the order connection
// (default: the private URL), api_key / api_secret / api_passphrase the three credentials, testnet
// = demo trading (the x-simulated-trading header). Extra keys: ws_private_url, td_mode ("cross" |
// "isolated"), depth_channel ("books" | "books50-l2-tbt" | "books-l2-tbt"), order_api ("ws" |
// "rest"), stale_ms, dead_ms, ping_interval_ms, orders_per_second, dead_mans_switch_s,
// position_from_stream, allow_offline_reference_data, cancel_on_order_channel_loss,
// emit_ack_from_response. Throws std::invalid_argument for a bad value, for api_key without
// api_passphrase outside a dry run, and for a demo host with testnet = false or the reverse.
OkxVenueConfig make_okx_config(const VenueSection& section, bool dry_run);

}  // namespace fastmm::venues::okx
