#pragma once
// CoinbaseAdvancedVenue: the Coinbase Advanced Trade connector for spot products (BTC-USD,
// ETH-USD, ...), the API of retail and individual Coinbase accounts, with a CDP API key.
//
// Channels on one reactor thread (AsyncAPI and OpenAPI specs, read 2026-09-30):
//   md    wss://advanced-trade-ws.coinbase.com         level2, market_trades, heartbeats; no key
//   user  wss://advanced-trade-ws-user.coinbase.com    user (JWT in the subscribe), heartbeats
//   rest  https://api.coinbase.com/api/v3/brokerage    products, time, orders, cancels, fills
// Orders go over REST (POST /orders, POST /orders/batch_cancel by the venue's order id); their
// states come from the user channel, and their executions from GET /orders/historical/fills,
// read for an order as soon as the user channel shows its cumulative quantity grew (the channel
// names no execution). cancel_all() uses an independent BlockingHttp connection.
//
// No test environment with matching: the Advanced Trade sandbox answers every request with a
// fixed reply (https://docs.cdp.coinbase.com/coinbase-app/advanced-trade-apis/sandbox), so the
// connector runs against production only (`testnet = false`).
//
// No venue-side dead man's switch: nothing in the Advanced Trade API cancels spot orders when a
// client disconnects. Orders left by a process that died stay until the next session's start-up
// sweep cancels them.
//
// Reconciliation: the account's fills of the subscribed products since the watermark (newest
// first, paged by cursor) are replayed with the trade_id as execution id; then the open orders
// (order_status=OPEN) become one Begin / OpenOrder* / End. A fill names its order by the venue's
// order_id: the connector maps the ones it knows and asks GET /orders/historical/<id> for the
// others. Spot has no positions to report.
#include "fastmm/config/config.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/net/connection.hpp"
#include "fastmm/venues/coinbase/advanced_auth.hpp"
#include "fastmm/venues/coinbase/advanced_error_map.hpp"
#include "fastmm/venues/coinbase/advanced_md_feed.hpp"
#include "fastmm/venues/coinbase/advanced_rest.hpp"
#include "fastmm/venues/coinbase/advanced_user_parser.hpp"
#include "fastmm/venues/coinbase/coinbase_wire.hpp"
#include "fastmm/venues/connection_slot.hpp"
#include "fastmm/venues/order_commands.hpp"
#include "fastmm/venues/rate_limiter.hpp"
#include "fastmm/venues/raw_recorder.hpp"
#include "fastmm/venues/reconcile_driver.hpp"
#include "fastmm/venues/replay_scheduler.hpp"
#include "fastmm/venues/rest_channel.hpp"
#include "fastmm/venues/venue.hpp"
#include "fastmm/venues/wire_latency.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fastmm::venues::coinbase {

struct AdvancedVenueConfig {
  std::string name = "coinbase";
  std::string ws_url;          // public feed
  std::string ws_private_url;  // the user channel
  std::string rest_url;        // https://api.coinbase.com
  CdpCredentials credentials;
  bool insecure_tls = false;
  std::string ca_file;
  bool dry_run = false;
  bool emit_ack_from_response = true;
  bool cancel_on_order_channel_loss = true;  // the user channel
  bool allow_offline_reference_data = false;
  std::uint32_t stale_ms = 2000;
  std::uint32_t dead_ms = 10'000;  // heartbeats every second on both connections
  std::uint32_t orders_per_second = 8;
  double rate_threshold = 0.9;
  // Orders a batch_cancel names; the documentation gives no maximum.
  std::uint32_t cancel_batch = 50;
  std::string record_raw_dir;
  std::uint32_t http_timeout_ms = 5000;
  net::BackoffConfig backoff{};
};

class CoinbaseAdvancedVenue final : public Venue, private ReconcileHooks {
 public:
  CoinbaseAdvancedVenue(VenueId id, AdvancedVenueConfig cfg);
  ~CoinbaseAdvancedVenue() override;

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

  [[nodiscard]] const AdvancedMdFeed* md_feed() const noexcept { return md_feed_.get(); }
  [[nodiscard]] const AdvancedVenueConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] bool fatal() const noexcept { return fatal_; }
  [[nodiscard]] std::size_t shadow_count() const noexcept { return shadows_.size(); }
  // The user channel is subscribed (reactor thread; tests).
  [[nodiscard]] bool order_channel_live() const noexcept { return user_conn_.is_live(); }
  [[nodiscard]] std::int64_t venue_time_ms() const noexcept;
  [[nodiscard]] const CdpJwtSigner& signer() const noexcept { return signer_; }

 private:
  struct MdHandler {
    CoinbaseAdvancedVenue* v;
    void on_state(net::ConnState s) { v->on_md_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_md_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_md_open(); }
  };
  struct UserHandler {
    CoinbaseAdvancedVenue* v;
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
    VenueOrderId venue_id{};  // known from the reply or the user channel
    Qty wanted{};             // cumulative quantity the user channel reported
    Qty emitted{};            // cumulative quantity of the executions forwarded
    std::int64_t next_fetch_ns = 0;
    std::uint8_t fetch_attempts = 0;
    bool fetching = false;
    bool fetch_again = false;
    bool cancel_parked = false;  // a cancel waits for the venue's order id
    bool ended = false;          // cancelled, expired or filled at the venue
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
  [[nodiscard]] net::ConnectionConfig ws_config(const std::string& url,
                                                bool manual_subscribe) const;
  [[nodiscard]] std::string rest_headers(std::string_view method,
                                         std::string_view target,
                                         bool body) const;
  template <class Ring>
  void write_orders(Ring& ring);
  void send_command(const OrderCommand& cmd);
  void send_cancel(ClientOrderId id, Shadow& s);
  void handle_new_reply(ClientOrderId id, const net::HttpResponse& r);
  void handle_cancel_reply(ClientOrderId id, const net::HttpResponse& r);
  void learn_venue_id(ClientOrderId id, std::string_view venue_id);
  // Reads the executions of an order whose cumulative quantity grew.
  void fetch_fills(ClientOrderId id);
  void maybe_forget(ClientOrderId id) noexcept;
  void emit_fill(const Shadow& s, ClientOrderId id, const AdvFillRow& f, Qty cum);
  // False when this trade id already went out from a per-order read.
  bool note_live_trade(const std::string& trade_id);
  void trip_venue_kill(KillReason reason);
  void apply_action(VenueAction action, int code, std::string_view msg);
  void request_resubscribe(InstrumentId id);
  void request_server_time();
  void cancel_all_async();
  void remember_order(std::string_view order_id, ClientOrderId cl) noexcept;
  [[nodiscard]] ClientOrderId name_of(std::string_view order_id) const noexcept;
  [[nodiscard]] std::vector<std::string> products() const;
  // ReconcileHooks: the open orders (paged), then the snapshot.
  bool fetch_snapshot(std::uint64_t generation) override;
  bool replay_executions() override;
  void shadow_ids(std::vector<SentShadow>& out) override;
  void drop_shadow(ClientOrderId id) override;
  bool request_open_orders_page(std::uint64_t generation, const std::string& cursor);
  [[nodiscard]] bool replay_ready() const noexcept;
  bool query_fills(const ReplayQuery& q);
  bool emit_replayed(const AdvFillRow& f);
  bool lookup_order(const ReplayLookup& l);
  void publish_status() noexcept;
  void refuse(const OrderCommand& cmd, RejectReason reason, std::string_view why);
  [[nodiscard]] InstrumentId subscribed_instrument(std::string_view product) const noexcept;
  [[nodiscard]] std::int64_t now_ns() const noexcept { return net::Reactor::now_ns(); }
  static void resubscribe_requester(void* ctx, InstrumentId id) noexcept {
    static_cast<CoinbaseAdvancedVenue*>(ctx)->request_resubscribe(id);
  }

  VenueId id_;
  AdvancedVenueConfig cfg_;
  CdpJwtSigner signer_;
  std::string rest_host_;  // the JWT's uri host
  const SymbolTable* symbols_ = nullptr;
  const InstrumentTable* instruments_ = nullptr;
  EventSink* md_sink_ = nullptr;
  EventSink* order_sink_ = nullptr;
  MsgRing* outbound_ = nullptr;
  net::Reactor* reactor_ = nullptr;

  std::unique_ptr<AdvancedMdFeed> md_feed_;
  std::unique_ptr<AdvancedUserParser> user_parser_;
  std::unique_ptr<AdvancedOrderEncoder> encoder_;
  std::unique_ptr<RestChannel> rest_;
  MdHandler md_handler_{this};
  UserHandler user_handler_{this};
  ConnectionSlot<MdHandler> md_conn_;
  ConnectionSlot<UserHandler> user_conn_;
  RateLimiter rate_;
  OpenHashMap<ClientOrderId, Shadow, kShadowSlots> shadows_;
  ShadowOverflow shadow_overflow_;
  OpenHashMap<OrderKey, ClientOrderId, kShadowSlots> order_names_;
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
  AdvancedUserResult user_result_{};
  RawRecorder raw_md_;
  RawRecorder raw_user_;

  std::unordered_set<std::string> live_trades_;
  std::deque<std::string> live_trade_order_;
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
  bool user_sequence_known_ = false;
  std::uint64_t user_sequence_ = 0;
  SentWatermark sent_;

  ReconcileDriver reconcile_{*this, sent_};
  std::size_t reconcile_pages_ = 0;
  std::vector<ClientOrderId> snapshot_ids_;

  ReplayScheduler<AdvFillRow> exec_replay_;
  ConnState md_state_ = ConnState::Disconnected;
  ConnState user_state_ = ConnState::Disconnected;
  net::TimerId housekeeping_timer_ = net::kInvalidTimer;
  std::uint64_t generation_ = 0;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);

  WireLatencyRecorder wire_;
  VenueStatus stats_{};
  Seqlocked<VenueStatus> published_{};
};

// [venues.<name>] -> AdvancedVenueConfig. ws_url = the public feed, rest_url, api_key = the CDP key
// name (organizations/<org>/apiKeys/<id>), api_secret = its EC private key PEM. Extra keys:
// ws_private_url (default: the production user endpoint), stale_ms, dead_ms, orders_per_second,
// cancel_batch, allow_offline_reference_data, cancel_on_order_channel_loss, emit_ack_from_response.
// Throws std::invalid_argument for a bad value, for a key that is not an EC P-256 PEM outside a
// dry run, for supports_replace = true, and for testnet = true with the production hosts.
AdvancedVenueConfig make_coinbase_advanced_config(const VenueSection& section, bool dry_run);

}  // namespace fastmm::venues::coinbase
