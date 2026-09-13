#pragma once
// BinanceVenue: the Binance Spot (testnet / Binance-compatible simulator) connector (6.4).
//
// Channels on one reactor thread:
//   md    wss://<stream>/stream?streams=...          combined market-data stream (BinanceMdFeed)
//   user  wss://<ws-api>/ws-api/v3                   user data stream via the WebSocket API
//         (userDataStream.subscribe.signature for HMAC keys, session.logon +
//         userDataStream.subscribe for Ed25519 keys), or the legacy listenKey stream
//         wss://<stream>/ws/<listenKey> for the local simulator (user_stream = "listen_key").
//         Binance testnet answers POST /api/v3/userDataStream with HTTP 410 since the
//         listenKey removal (CHANGELOG 2025-04-07 / 2025-08-12), observed 2026-09-13.
//   order wss://<ws-api>/ws-api/v3                   order.place / order.cancel /
//         order.cancelReplace / openOrders.cancelAll / openOrders.status, REST fallback
//   rest  https://<rest>                             exchangeInfo, depth snapshots, time,
//         openOrders, listenKey keepalive (RestChannel on the reactor)
// cancel_all() uses an independent BlockingHttp connection so the kill switch works even if
// the reactor thread is wedged (6.7).
//
// Rate limits: REQUEST_WEIGHT / ORDERS buckets from exchangeInfo.rateLimits; WS API
// responses carry `rateLimits[]` and REST responses X-MBX-USED-WEIGHT-1M /
// X-MBX-ORDER-COUNT-10S; 429 -> Retry-After cooldown; 418 -> REST hard stop.
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/net/connection.hpp"
#include "fastmm/venues/binance/binance_auth.hpp"
#include "fastmm/venues/binance/binance_error_map.hpp"
#include "fastmm/venues/binance/binance_md_feed.hpp"
#include "fastmm/venues/binance/binance_order_encoder.hpp"
#include "fastmm/venues/binance/binance_user_parser.hpp"
#include "fastmm/venues/connection_slot.hpp"
#include "fastmm/venues/rate_limiter.hpp"
#include "fastmm/venues/raw_recorder.hpp"
#include "fastmm/venues/rest_channel.hpp"
#include "fastmm/venues/venue.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::binance {

enum class UserStreamMode : std::uint8_t {
  Auto = 0,       // WsApi when credentials are usable, None otherwise
  WsApi = 1,      // userDataStream.subscribe(.signature) on a WS API connection
  ListenKey = 2,  // POST /api/v3/userDataStream + /ws/<listenKey> (simulator)
  None = 3,       // dry-run
};

struct BinanceVenueConfig {
  std::string name = "binance";
  std::string ws_url;      // stream base: wss://stream.testnet.binance.vision[/stream]
  std::string ws_api_url;  // wss://ws-api.testnet.binance.vision/ws-api/v3
  std::string rest_url;    // https://testnet.binance.vision
  Credentials credentials;
  int recv_window_ms = kDefaultRecvWindowMs;
  bool insecure_tls = false;
  std::string ca_file;
  UserStreamMode user_stream = UserStreamMode::Auto;
  bool dry_run = false;      // public market data only: no user/order channels
  bool ws_order_api = true;  // false: REST order entry only
  bool emit_ack_from_response = true;
  bool position_from_balance = false;  // forward outboundAccountPosition as PositionUpdate
  bool cancel_on_order_channel_loss = true;
  bool allow_offline_reference_data = false;  // keep config tick/lot if exchangeInfo fails
  bool supports_replace = true;               // cancelReplace
  int depth_limit = 1000;                     // GET /api/v3/depth limit (weight 50)
  std::uint32_t stale_ms = 2000;
  std::uint32_t dead_ms = 10'000;
  std::uint64_t max_lifetime_ms = 23ULL * 3600 * 1000;  // roll over before the 24 h cut
  double rate_threshold = 0.9;
  std::int64_t min_snapshot_interval_ns = BinanceDepthSync::kDefaultMinInterval;
  std::string record_raw_dir;
  std::uint32_t http_timeout_ms = 5000;
  net::BackoffConfig backoff{};
};

class BinanceVenue final : public Venue {
 public:
  BinanceVenue(VenueId id, BinanceVenueConfig cfg);
  ~BinanceVenue() override;

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
  void on_wake() override;
  void request_open_orders() override;
  bool cancel_all() override;
  [[nodiscard]] VenueStatus status() const noexcept override;

  // ---- introspection (tests / stats) --------------------------------------------------------
  [[nodiscard]] const BinanceMdFeed* md_feed() const noexcept { return md_feed_.get(); }
  [[nodiscard]] const RateLimiter& rate_limiter() const noexcept { return rate_; }
  [[nodiscard]] std::int64_t clock_offset_ms() const noexcept { return clock_offset_ms_; }
  [[nodiscard]] const BinanceVenueConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] bool fatal() const noexcept { return fatal_; }
  // Venue time in ms (local wall clock + measured offset).
  [[nodiscard]] std::int64_t venue_time_ms() const noexcept;

 private:
  enum class Channel : std::uint8_t { Md = 0, User = 1, Order = 2 };

  struct MdHandler {
    BinanceVenue* v;
    void on_state(net::ConnState s) { v->on_md_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_md_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_md_open(); }
  };
  struct UserHandler {
    BinanceVenue* v;
    void on_state(net::ConnState s) { v->on_user_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_user_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_user_open(); }
  };
  struct OrderHandler {
    BinanceVenue* v;
    void on_state(net::ConnState s) { v->on_order_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_order_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_order_open(); }
  };
  friend struct MdHandler;
  friend struct UserHandler;
  friend struct OrderHandler;

  // channel callbacks (reactor thread)
  void on_md_state(net::ConnState s);
  void on_md_text(std::string_view t, std::int64_t ts);
  void on_md_open();
  void on_user_state(net::ConnState s);
  void on_user_text(std::string_view t, std::int64_t ts);
  void on_user_open();
  void on_order_state(net::ConnState s);
  void on_order_text(std::string_view t, std::int64_t ts);
  void on_order_open();

  // helpers
  void open_md();
  void open_user();
  void open_order();
  void open_rest();
  void emit_connection_state(Channel ch, ConnState state, std::int32_t reason = 0);
  void drain_outbound();
  void send_command(const OrderCommand& cmd);
  void send_command_rest(const OrderCommand& cmd, const OrderShadow* shadow);
  void handle_ws_api_response(const WsApiResponse& r, std::string_view raw);
  void handle_order_response(RequestKind kind, ClientOrderId id, const WsApiResponse& r);
  void handle_rest_order_response(const OrderCommand& cmd, const net::HttpResponse& r);
  void apply_action(VenueAction action,
                    int code,
                    std::string_view msg,
                    std::int64_t retry_after_ms);
  void emit_reject(
      InstrumentId inst, ClientOrderId id, RejectReason reason, int code, std::string_view text);
  void emit_cancel_reject(
      InstrumentId inst, ClientOrderId id, RejectReason reason, int code, std::string_view text);
  void emit_ack(InstrumentId inst, ClientOrderId id, std::int64_t order_id);
  void emit_cancel_ack(InstrumentId inst,
                       ClientOrderId id,
                       std::int64_t order_id,
                       std::string_view executed_qty);
  void request_snapshot(InstrumentId id);
  void request_server_time();
  void request_listen_key();
  void keepalive_listen_key();
  void cancel_all_async();
  void emit_reconcile(std::string_view json, bool rest_array);
  void publish_status() noexcept;
  void note_rate_headers(const net::HttpResponse& r);
  [[nodiscard]] std::int64_t now_ns() const noexcept { return net::Reactor::now_ns(); }
  static void snapshot_requester(void* ctx, InstrumentId id) noexcept {
    static_cast<BinanceVenue*>(ctx)->request_snapshot(id);
  }
  [[nodiscard]] InstrumentId instrument_of(std::string_view symbol) const noexcept;
  [[nodiscard]] std::string api_headers() const;
  net::ConnectionConfig ws_config(const std::string& url, bool manual_subscribe) const;

  VenueId id_;
  BinanceVenueConfig cfg_;
  Signer signer_;
  const SymbolTable* symbols_ = nullptr;
  const InstrumentTable* instruments_ = nullptr;
  EventSink* md_sink_ = nullptr;
  EventSink* order_sink_ = nullptr;
  MsgRing* outbound_ = nullptr;
  net::Reactor* reactor_ = nullptr;

  std::unique_ptr<BinanceMdFeed> md_feed_;
  std::unique_ptr<BinanceUserParser> user_parser_;
  std::unique_ptr<BinanceOrderEncoder> encoder_;
  std::unique_ptr<BinanceWsApiDecoder> ws_api_decoder_;
  std::unique_ptr<RestChannel> rest_;
  MdHandler md_handler_{this};
  UserHandler user_handler_{this};
  OrderHandler order_handler_{this};
  ConnectionSlot<MdHandler> md_conn_;
  ConnectionSlot<UserHandler> user_conn_;
  ConnectionSlot<OrderHandler> order_conn_;
  RateLimiter rate_;
  OpenHashMap<ClientOrderId, OrderShadow, 8192> shadows_;
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
  char request_buf_[kMaxRequestBytes];
  RawRecorder raw_md_;
  RawRecorder raw_user_;
  RawRecorder raw_order_;

  std::vector<InstrumentId> subscribed_;
  std::string listen_key_;
  std::int64_t listen_key_refresh_ns_ = 0;
  std::atomic<std::int64_t> clock_offset_ms_{0};  // read by cancel_all() from any thread
  std::int64_t clock_sync_ns_ = 0;
  std::int64_t time_request_sent_ns_ = 0;
  bool time_request_pending_ = false;
  bool clock_resync_wanted_ = false;
  bool session_logged_on_ = false;
  bool user_subscribed_ = false;
  bool fatal_ = false;
  bool connected_ = false;
  bool rest_hard_stopped_ = false;
  ConnState md_state_ = ConnState::Disconnected;
  ConnState user_state_ = ConnState::Disconnected;
  ConnState order_state_ = ConnState::Disconnected;
  bool order_was_live_ = false;
  bool user_was_live_ = false;
  net::TimerId housekeeping_timer_ = net::kInvalidTimer;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);

  WireLatencyRecorder wire_;  // reactor thread; summarized into stats_ by publish_status()
  VenueStatus stats_{};
  Seqlocked<VenueStatus> published_{};
};

// Builds a BinanceVenueConfig from a config section (fastmm::VenueSection); shared by the
// live app and the tests. `extra` keys: user_stream, key_type, private_key_file,
// depth_limit, position_from_balance, stale_ms, dead_ms, order_api ("ws"|"rest"),
// allow_offline_reference_data, cancel_on_order_channel_loss.
struct VenueSectionView {
  std::string name;
  std::string ws_url;
  std::string ws_api_url;
  std::string rest_url;
  std::string api_key;
  std::string api_secret;
  bool supports_replace = true;
  bool insecure_tls = false;
  std::string ca_file;
  int recv_window_ms = 3000;
  const std::map<std::string, std::string>* extra = nullptr;
};
BinanceVenueConfig make_binance_config(const VenueSectionView& section, bool dry_run);

}  // namespace fastmm::venues::binance
