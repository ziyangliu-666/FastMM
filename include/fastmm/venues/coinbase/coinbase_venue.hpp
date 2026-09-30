#pragma once
// CoinbaseExchangeVenue: the Coinbase Exchange connector for spot products (BTC-USD, ETH-USD, ...).
//
// Channels on one reactor thread (https://docs.cdp.coinbase.com/exchange/introduction/sandbox and
// .../websocket-feed/overview, read 2026-09-30):
//   md    wss://ws-feed.exchange.coinbase.com            level2_batch (or level2, signed), matches,
//         (sandbox wss://ws-feed-public.sandbox.         heartbeat
//          exchange.coinbase.com)
//   user  the same URL, a second connection              user + heartbeat, signed subscribe
//   rest  https://api.exchange.coinbase.com              products, time, orders, fills, cancels
//         (sandbox https://api-public.sandbox.
//          exchange.coinbase.com)
// Orders go over REST (POST /orders, DELETE /orders/client:<client_oid>); their events come from
// the user channel. cancel_all() uses an independent BlockingHttp connection.
//
// No venue-side dead man's switch: the Exchange cancels on disconnect only for orders of a FIX
// order-entry session (Logon tag 8013 CancelOrdersOnDisconnect), and nothing on REST or the
// WebSocket feed arms one. Orders left by a process that died stay until the next session's
// start-up sweep cancels them.
//
// Reconciliation: the fills of every subscribed product since the watermark (GET /fills, newest
// first, paged by the CB-AFTER cursor) are replayed as fills with the trade_id as execution id;
// then GET /orders (open, pending, active) becomes one Begin / OpenOrder* / End. A fill names its
// order by the venue's order id: the connector maps the ones it knows and asks GET /orders/<id>
// for the others (ReplayScheduler lookups). Spot has no positions to report.
//
// Balances: GET /accounts after every snapshot (ReconcileDriver's balance leg), stamped with the
// venue clock when it arrived (the reply carries no time). Its account ids of the tracked assets
// subscribe the `balance` channel on the user connection, whose updates go out as they come,
// stamped with their `updated` time. The channel "does not track every update"
// (websocket-feed/channels, Balance Channel), so a fill also asks for GET /accounts again, from
// the housekeeping timer (at most once a second).
#include "fastmm/config/config.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/net/connection.hpp"
#include "fastmm/venues/coinbase/coinbase_auth.hpp"
#include "fastmm/venues/coinbase/coinbase_error_map.hpp"
#include "fastmm/venues/coinbase/coinbase_md_feed.hpp"
#include "fastmm/venues/coinbase/coinbase_order_encoder.hpp"
#include "fastmm/venues/coinbase/coinbase_private_parser.hpp"
#include "fastmm/venues/coinbase/coinbase_rest_decoder.hpp"
#include "fastmm/venues/connection_slot.hpp"
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
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::coinbase {

struct CoinbaseVenueConfig {
  std::string name = "coinbase";
  std::string ws_url;          // public feed: market data
  std::string ws_private_url;  // the user channel; ws_url by default
  std::string rest_url;
  Credentials credentials;
  bool sandbox = true;  // the section's `testnet`: the sandbox hosts
  DepthChannel depth_channel = DepthChannel::Level2Batch;
  Stp stp = Stp::Dc;
  bool insecure_tls = false;
  std::string ca_file;
  bool dry_run = false;
  bool emit_ack_from_response = true;
  bool cancel_on_order_channel_loss = true;  // the user channel: orders are reported there
  bool allow_offline_reference_data = false;
  std::uint32_t stale_ms = 2000;
  std::uint32_t dead_ms = 10'000;  // heartbeats come every second on both connections
  // New orders per second, client side. The venue allows 15 private REST requests a second per
  // profile (bursts of 30), cancels and queries included.
  std::uint32_t orders_per_second = 8;
  double rate_threshold = 0.9;
  // DELETE /orders is best effort: asked again until it cancels nothing, at most this often.
  int cancel_all_rounds = 3;
  std::string record_raw_dir;
  std::uint32_t http_timeout_ms = 5000;
  net::BackoffConfig backoff{};
};

class CoinbaseExchangeVenue final : public Venue, private ReconcileHooks {
 public:
  CoinbaseExchangeVenue(VenueId id, CoinbaseVenueConfig cfg);
  ~CoinbaseExchangeVenue() override;

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
  bool cancel_all() override;
  [[nodiscard]] VenueStatus status() const noexcept override;

  [[nodiscard]] const CoinbaseMdFeed* md_feed() const noexcept { return md_feed_.get(); }
  [[nodiscard]] const CoinbaseVenueConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] bool fatal() const noexcept { return fatal_; }
  [[nodiscard]] std::size_t shadow_count() const noexcept { return shadows_.size(); }
  // The user channel is subscribed: order events reach the engine (reactor thread; tests).
  [[nodiscard]] bool order_channel_live() const noexcept { return user_conn_.is_live(); }
  [[nodiscard]] std::int64_t clock_offset_ms() const noexcept { return clock_offset_ms_.load(); }
  [[nodiscard]] std::int64_t venue_time_ms() const noexcept;
  [[nodiscard]] const CoinbasePrivateParser* private_parser() const noexcept {
    return private_parser_.get();
  }

 private:
  struct MdHandler {
    CoinbaseExchangeVenue* v;
    void on_state(net::ConnState s) { v->on_md_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_md_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_md_open(); }
  };
  struct UserHandler {
    CoinbaseExchangeVenue* v;
    void on_state(net::ConnState s) { v->on_user_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_user_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_user_open(); }
  };

  // What the connector keeps of an order it sent.
  struct Shadow {
    InstrumentId instrument{};
    Side side = Side::Buy;
    Qty qty{};
    std::uint64_t sent_seq = 0;
  };

  void on_md_state(net::ConnState s);
  void on_md_text(std::string_view t, std::int64_t ts);
  void on_md_open();
  void on_user_state(net::ConnState s);
  void on_user_text(std::string_view t, std::int64_t ts);
  void on_user_open();

  void open_rest();
  void open_md();
  void open_user();
  [[nodiscard]] bool md_needs_auth() const noexcept;
  [[nodiscard]] net::ConnectionConfig ws_config(const std::string& url,
                                                bool manual_subscribe,
                                                std::uint32_t dead_ms) const;
  // `payload` (a subscribe or unsubscribe object) with the WebSocket signature fields added.
  [[nodiscard]] std::string signed_payload(std::string_view payload) const;
  [[nodiscard]] std::string rest_headers(std::string_view method,
                                         std::string_view target,
                                         std::string_view body) const;
  template <class Ring>
  void write_orders(Ring& ring);
  void send_command(const OrderCommand& cmd);
  void handle_new_reply(ClientOrderId id, const net::HttpResponse& r);
  void handle_cancel_reply(ClientOrderId id, const net::HttpResponse& r);
  void trip_venue_kill(KillReason reason);
  void apply_action(VenueAction action, int code, std::string_view msg);
  void request_resubscribe(InstrumentId id);
  void request_server_time();
  void cancel_all_async();
  void remember_order(std::string_view order_id, ClientOrderId cl) noexcept;
  [[nodiscard]] ClientOrderId name_of(std::string_view order_id) const noexcept;
  // ReconcileHooks: GET /orders (paged), then the snapshot.
  bool fetch_snapshot(std::uint64_t generation) override;
  bool replay_executions() override;
  void shadow_ids(std::vector<SentShadow>& out) override;
  void drop_shadow(ClientOrderId id) override;
  bool request_open_orders_page(std::uint64_t generation, const std::string& after);
  // ReconcileHooks: GET /accounts, then the balance snapshot.
  bool fetch_balances(std::uint64_t generation) override;
  // The balance channel for the accounts of the tracked assets, on the user connection.
  void subscribe_balances();
  // Execution replay (ReplayScheduler over GET /fills, one stream per product).
  [[nodiscard]] bool replay_ready() const noexcept;
  bool query_fills(const ReplayQuery& q);
  bool emit_fill(std::size_t stream, const FillRow& f);
  bool lookup_order(const ReplayLookup& l);
  void publish_status() noexcept;
  void refuse(const OrderCommand& cmd, RejectReason reason, std::string_view why);
  void forget_order(ClientOrderId id) noexcept;
  [[nodiscard]] InstrumentId subscribed_instrument(std::string_view product) const noexcept;
  [[nodiscard]] std::int64_t now_ns() const noexcept { return net::Reactor::now_ns(); }
  static void resubscribe_requester(void* ctx, InstrumentId id) noexcept {
    static_cast<CoinbaseExchangeVenue*>(ctx)->request_resubscribe(id);
  }

  VenueId id_;
  CoinbaseVenueConfig cfg_;
  Signer signer_;
  const SymbolTable* symbols_ = nullptr;
  const InstrumentTable* instruments_ = nullptr;
  EventSink* md_sink_ = nullptr;
  EventSink* order_sink_ = nullptr;
  MsgRing* outbound_ = nullptr;
  net::Reactor* reactor_ = nullptr;

  std::unique_ptr<CoinbaseMdFeed> md_feed_;
  std::unique_ptr<CoinbasePrivateParser> private_parser_;
  std::unique_ptr<CoinbaseOrderEncoder> encoder_;
  std::unique_ptr<RestChannel> rest_;
  MdHandler md_handler_{this};
  UserHandler user_handler_{this};
  ConnectionSlot<MdHandler> md_conn_;
  ConnectionSlot<UserHandler> user_conn_;
  RateLimiter rate_;
  OpenHashMap<ClientOrderId, Shadow, kShadowSlots> shadows_;
  ShadowOverflow shadow_overflow_;
  // Venue order id -> client order id, for fills replayed after the user channel forgot the order.
  OpenHashMap<OrderKey, ClientOrderId, kShadowSlots> order_names_;
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
  RawRecorder raw_md_;
  RawRecorder raw_user_;

  std::vector<InstrumentId> subscribed_;
  std::atomic<std::int64_t> clock_offset_ms_{0};
  std::int64_t clock_sync_ns_ = 0;
  bool time_request_pending_ = false;
  bool clock_resync_wanted_ = false;
  bool fatal_ = false;
  bool venue_kill_sent_ = false;
  bool connected_ = false;
  bool rest_hard_stopped_ = false;
  bool user_was_live_ = false;
  SentWatermark sent_;

  ReconcileDriver reconcile_{*this, sent_};
  std::size_t reconcile_pages_ = 0;
  std::vector<ClientOrderId> snapshot_ids_;    // the client ids of the snapshot being fetched
  std::vector<std::string> balance_accounts_;  // account ids of the tracked assets (GET /accounts)
  std::vector<std::string> balance_subscribed_;  // those the user connection has subscribed
  bool balances_after_fill_ = false;             // a fill came: the housekeeping timer refreshes

  ReplayScheduler<FillRow> exec_replay_;
  ConnState md_state_ = ConnState::Disconnected;
  ConnState user_state_ = ConnState::Disconnected;
  net::TimerId housekeeping_timer_ = net::kInvalidTimer;
  std::uint64_t generation_ = 0;  // replies of requests a disconnect() abandoned are ignored
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);

  WireLatencyRecorder wire_;
  VenueStatus stats_{};
  Seqlocked<VenueStatus> published_{};
};

// [venues.<name>] -> CoinbaseVenueConfig. ws_url = the public feed, rest_url, api_key / api_secret
// / api_passphrase the three credentials, testnet = the sandbox. Extra keys: ws_private_url,
// depth_channel ("level2_batch" | "level2"), stp ("dc" | "co" | "cn" | "cb"), stale_ms, dead_ms,
// orders_per_second, cancel_all_rounds, allow_offline_reference_data,
// cancel_on_order_channel_loss, emit_ack_from_response. Throws std::invalid_argument for a bad
// value, for api_key without api_passphrase or with a secret that is not base64 outside a dry run,
// for supports_replace = true, and for a sandbox host with testnet = false or the reverse.
CoinbaseVenueConfig make_coinbase_config(const VenueSection& section, bool dry_run);

}  // namespace fastmm::venues::coinbase
