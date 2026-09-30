#pragma once
// GeminiVenue: the Gemini connector for perpetuals (product_type swap, contract_type linear, e.g.
// btcgusdperp) and spot (btcusd), one API for both.
//
// Channels on one reactor thread (https://developer.gemini.com/websocket/introduction.md, read
// 2026-09-30; sandbox wss://ws.sandbox.gemini.com and https://api.sandbox.gemini.com):
//   md     wss://ws.gemini.com?snapshot=-1          {sym}@depth@100ms, {sym}@bookTicker,
//                                                   {sym}@trade; `time` for the clock offset
//   order  wss://ws.gemini.com?cancelOnDisconnect=true
//                                                   authenticated at the upgrade (signed headers);
//                                                   orders@account, balances@account,
//                                                   order.place, order.cancel
//   rest   https://api.gemini.com                   symbols/details (public), orders, positions,
//                                                   balances, margin,
//                                                   mytrades, perpetuals/fundingPayment,
//                                                   order/cancel, order/cancel/session, heartbeat
// The WebSocket API sends no heartbeats: the connector sends `ping` on both connections every
// ping_interval_ms and a connection that has heard nothing for dead_ms reconnects.
//
// Units. A perpetual is 1 base unit per contract ("Each contract has an underlying of 1 BTC",
// https://www.gemini.com/artemis/legal/contract-specifications): quantities are base units and
// contract_multiplier is 1. tick = quote_increment, lot = tick_size (Gemini's name for the
// quantity step), min_qty = min_order_size; base = base_currency, quote = quote_currency.
//
// Order entry is order.place and order.cancel on the order connection. No amend exists
// (caps.supports_replace = false) and no reduce-only: a reduce_only order goes out as a plain
// one. order.cancel takes the venue's id only, so a cancel of an order whose NEW event has not
// arrived waits for it. With the order connection down, a new order is refused and a cancel goes
// to POST /v1/order/cancel.
//
// Dead man's switch: cancelOnDisconnect=true on the order connection (the venue cancels the
// orders placed on it when it goes away, as Deribit's cancel-on-disconnect). With `heartbeat`, the
// API key's "Requires Heartbeat" setting is served as well: POST /v1/heartbeat every 15 s (the
// venue cancels the key's orders after 30 s without an authenticated request), a CountdownDriver
// whose lapse kills the venue.
//
// Reconciliation: the trades since the watermark (mytrades per symbol) are replayed as fills with
// the tid as execution id; then /v1/orders and, with a perpetual subscribed, /v1/positions become
// one Begin / OpenOrder* / Position* / End (a perpetual absent from the positions is flat; spot has
// no position rows). Funding: /v1/perpetuals/fundingPayment, one FundingMsg per hourly transfer.
//
// Balances: balances@account on the order connection (every change, the assets that changed, each
// absolute) as BalanceMsg stamped with its `u`; the reconciliation's balance leg is /v1/balances
// (spot rows: free = available, locked = amount - available) and, with a perpetual subscribed,
// /v1/margin as the account row (kAccount, USD). The margin has no stream: a perpetual fill asks
// for the balance leg again (at most once a second), as does any fill while balances@account is
// not subscribed. An exchange account answers /v1/margin AccountNotOfTypeRequired: no account row,
// and the leg goes on without it.
#include "fastmm/config/config.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/net/connection.hpp"
#include "fastmm/venues/connection_slot.hpp"
#include "fastmm/venues/dead_mans_switch.hpp"
#include "fastmm/venues/error_action.hpp"
#include "fastmm/venues/gemini/gemini_auth.hpp"
#include "fastmm/venues/gemini/gemini_md_feed.hpp"
#include "fastmm/venues/gemini/gemini_order_encoder.hpp"
#include "fastmm/venues/gemini/gemini_private_parser.hpp"
#include "fastmm/venues/gemini/gemini_rest_decoder.hpp"
#include "fastmm/venues/order_commands.hpp"
#include "fastmm/venues/rate_limiter.hpp"
#include "fastmm/venues/raw_recorder.hpp"
#include "fastmm/venues/reconcile_driver.hpp"
#include "fastmm/venues/replay_scheduler.hpp"
#include "fastmm/venues/rest_channel.hpp"
#include "fastmm/venues/venue.hpp"
#include "fastmm/venues/wire_latency.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::gemini {

struct GeminiVenueConfig {
  std::string name = "gemini";
  std::string ws_md_url;     // wss://ws.gemini.com; the connector adds snapshot=-1
  std::string ws_order_url;  // the md URL by default; the connector adds cancelOnDisconnect
  std::string rest_url;      // https://api.gemini.com
  Credentials credentials;
  bool sandbox = true;
  bool insecure_tls = false;
  std::string ca_file;
  bool dry_run = false;
  bool cancel_on_disconnect = true;
  bool heartbeat = false;  // the key has "Requires Heartbeat": POST /v1/heartbeat every 15 s
  bool cancel_on_order_channel_loss = true;
  bool allow_offline_reference_data = false;
  std::uint32_t stale_ms = 2000;
  std::uint32_t dead_ms = 30'000;
  std::uint32_t ping_interval_ms = 10'000;
  std::uint32_t orders_per_second = 20;
  double rate_threshold = 0.9;
  std::string record_raw_dir;
  std::uint32_t http_timeout_ms = 5000;
  net::BackoffConfig backoff{};
};

// Per order sent: what a cancel and the replies need that the engine message does not carry.
struct OrderShadow {
  InstrumentId instrument{};
  Side side = Side::Buy;
  bool cancel_pending = false;  // cancelled before the venue named it: sent once it does
  std::uint64_t sent_seq = 0;   // SentWatermark send sequence of its New
  FixedString<24> venue_id{};   // `i` of its NEW event (or order.place's result)
};

class GeminiVenue final : public Venue, private ReconcileHooks {
 public:
  // Heartbeat: "If no authenticated request or explicit Heartbeat message is received for 30
  // seconds, the exchange automatically cancels all outstanding open orders for that session".
  static constexpr std::int64_t kHeartbeatWindowMs = 30'000;

  GeminiVenue(VenueId id, GeminiVenueConfig cfg);
  ~GeminiVenue() override;

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

  [[nodiscard]] const GeminiMdFeed* md_feed() const noexcept { return md_feed_.get(); }
  [[nodiscard]] const GeminiVenueConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] bool fatal() const noexcept { return fatal_; }
  [[nodiscard]] std::size_t shadow_count() const noexcept { return shadows_.size(); }
  // The order connection is up and subscribed to orders@account (reactor thread; tests).
  [[nodiscard]] bool order_channel_live() const noexcept { return order_conn_.is_live(); }
  [[nodiscard]] std::int64_t clock_offset_ms() const noexcept { return clock_offset_ms_.load(); }
  [[nodiscard]] std::int64_t venue_time_ms() const noexcept;
  // The md and order connection URLs as connected (tests).
  [[nodiscard]] std::string md_url() const;
  [[nodiscard]] std::string order_url() const;

 private:
  struct MdHandler {
    GeminiVenue* v;
    void on_state(net::ConnState s) { v->on_md_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_md_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_md_open(); }
  };
  struct OrderHandler {
    GeminiVenue* v;
    void on_state(net::ConnState s) { v->on_order_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_order_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_order_open(); }
  };

  void on_md_state(net::ConnState s);
  void on_md_text(std::string_view t, std::int64_t ts);
  void on_md_open();
  void on_order_state(net::ConnState s);
  void on_order_text(std::string_view t, std::int64_t ts);
  void on_order_open();
  void on_order_event(EventHeader& h);
  void on_order_reply(const PrivateControl& c);

  [[nodiscard]] net::ConnectionConfig ws_config(const std::string& url) const;
  // Venue time in ms, strictly above every nonce sent before (any thread: cancel_all).
  [[nodiscard]] std::int64_t next_nonce() noexcept;
  [[nodiscard]] std::string ws_auth_headers();
  // Queues a signed REST request; `done` gets the reply unless the connector went away.
  bool rest_post(const RestRequest& rr, std::function<void(const net::HttpResponse&)> done);
  void drain_outbound();
  template <class Ring>
  void write_orders(Ring& ring);
  void send_command(const OrderCommand& cmd);
  void send_cancel(const OrderCommand& cmd, std::string_view venue_id);
  void send_cancel_rest(InstrumentId inst, ClientOrderId id, std::string_view venue_id);
  void fail_batch();
  void trip_venue_kill(KillReason reason);
  void apply_action(VenueAction action, int code, std::string_view msg);
  void apply_rest_error(const net::HttpResponse& r, std::string_view what);
  void request_resubscribe(InstrumentId id);
  void request_server_time();
  void cancel_session_async();
  void send_heartbeat();
  std::string check_key(bool perpetuals);
  // ReconcileHooks: /v1/orders, then /v1/positions, then the snapshot.
  bool fetch_snapshot(std::uint64_t generation) override;
  bool replay_executions() override;
  void shadow_ids(std::vector<SentShadow>& out) override;
  void drop_shadow(ClientOrderId id) override;
  bool request_positions(std::uint64_t generation);
  // The balance leg: /v1/balances, then /v1/margin with a perpetual subscribed.
  bool fetch_balances(std::uint64_t generation) override;
  void request_margin(std::uint64_t generation, std::int64_t venue_ms);
  // A balanceUpdate of balances@account.
  void on_balance_update(const BalanceUpdate& b);
  // Execution replay (mytrades per symbol) and funding (fundingPayment).
  [[nodiscard]] bool replay_ready() const noexcept;
  bool query_trades(const ReplayQuery& q);
  bool emit_trade(std::size_t stream, const TradeRow& t);
  bool query_funding(const ReplayQuery& q);
  bool emit_funding_row(const FundingRow& f);
  void publish_status() noexcept;
  void refuse_untracked(const OrderCommand& cmd);
  [[nodiscard]] InstrumentId subscribed_instrument(std::string_view symbol) const noexcept;
  [[nodiscard]] bool is_perpetual(InstrumentId id) const noexcept;
  [[nodiscard]] bool any_perpetual() const noexcept;
  [[nodiscard]] std::int64_t now_ns() const noexcept { return net::Reactor::now_ns(); }
  static void resubscribe_requester(void* ctx, InstrumentId id) noexcept {
    static_cast<GeminiVenue*>(ctx)->request_resubscribe(id);
  }

  VenueId id_;
  GeminiVenueConfig cfg_;
  Signer signer_;
  const SymbolTable* symbols_ = nullptr;
  const InstrumentTable* instruments_ = nullptr;
  EventSink* md_sink_ = nullptr;
  EventSink* order_sink_ = nullptr;
  MsgRing* outbound_ = nullptr;
  net::Reactor* reactor_ = nullptr;

  std::unique_ptr<GeminiMdFeed> md_feed_;
  std::unique_ptr<GeminiPrivateParser> private_parser_;
  std::unique_ptr<GeminiOrderEncoder> encoder_;
  std::unique_ptr<RestChannel> rest_;
  MdHandler md_handler_{this};
  OrderHandler order_handler_{this};
  ConnectionSlot<MdHandler> md_conn_;
  ConnectionSlot<OrderHandler> order_conn_;
  RateLimiter rate_;
  CountdownDriver heartbeat_;
  OpenHashMap<ClientOrderId, OrderShadow, kShadowSlots> shadows_;
  ShadowOverflow shadow_overflow_;
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
  char request_buf_[kMaxRequestBytes];
  RawRecorder raw_md_;
  RawRecorder raw_order_;

  std::vector<InstrumentId> subscribed_;
  std::atomic<std::int64_t> clock_offset_ms_{0};
  std::int64_t clock_sync_ns_ = 0;
  std::int64_t last_ping_ns_ = 0;
  std::atomic<std::int64_t> last_nonce_{0};
  bool time_request_pending_ = false;
  bool fatal_ = false;
  bool venue_kill_sent_ = false;
  bool connected_ = false;
  bool rest_hard_stopped_ = false;
  bool order_was_live_ = false;
  bool refused_account_settings_ = false;
  bool balance_stream_ = false;     // balances@account subscribed on the order connection
  bool no_margin_account_ = false;  // /v1/margin answered AccountNotOfTypeRequired
  SentWatermark sent_;
  BatchedOrders batch_;

  ReconcileDriver reconcile_{*this, sent_};
  // Execution replay per subscribed symbol (stream i = subscribed_[i]) and funding, one stream.
  ReplayScheduler<TradeRow> exec_replay_;
  ReplayScheduler<FundingRow> funding_replay_;
  ConnState md_state_ = ConnState::Disconnected;
  ConnState order_state_ = ConnState::Disconnected;
  net::TimerId housekeeping_timer_ = net::kInvalidTimer;
  std::uint64_t generation_ = 0;  // replies of requests a disconnect() abandoned are ignored
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);

  WireLatencyRecorder wire_;
  VenueStatus stats_{};
  Seqlocked<VenueStatus> published_{};
};

// [venues.<name>] -> GeminiVenueConfig. ws_url = the WebSocket API (market data), ws_api_url =
// the order connection (default: ws_url), rest_url; api_key / api_secret an account-scoped key
// with a time-based nonce; testnet = the sandbox. Extra keys: cancel_on_disconnect, heartbeat,
// stale_ms, dead_ms, ping_interval_ms, orders_per_second, allow_offline_reference_data,
// cancel_on_order_channel_loss. Throws std::invalid_argument for a bad value, a master key, and a
// sandbox host with testnet = false or the reverse.
GeminiVenueConfig make_gemini_config(const VenueSection& v, bool dry_run);

}  // namespace fastmm::venues::gemini
