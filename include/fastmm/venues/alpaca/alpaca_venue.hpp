#pragma once
// AlpacaVenue: US stocks through Alpaca's Trading API (paper or live account).
//
// Channels on one reactor thread:
//   md     wss://stream.data.alpaca.markets/v2/iex   quotes and trades (alpaca_md.hpp); one
//          connection per account on the free plan, so a second process is refused (406)
//   trade  wss://paper-api.alpaca.markets/stream     trade_updates: order states and executions
//   rest   https://paper-api.alpaca.markets          orders, replaces, cancels, open orders,
//          positions, fill activities, assets, clock (alpaca_rest.hpp)
// Both WebSockets authenticate with a frame carrying the key and the secret; REST requests carry
// them as headers. cancel_all() uses an independent BlockingHttp connection.
//
// Orders: POST, PATCH (a replace is a new order with a new id: the reply acknowledges the new
// client id, the old order ends "replaced") and DELETE by the venue's order id. Executions come
// from trade_updates with their execution_id; a reconciliation replays the account's FILL
// activities first (their id ends in the same execution_id, so nothing is booked twice), then sends
// the open orders and the position of every subscribed symbol, flat ones as zero.
//
// Venue rules a strategy meets as rejects (map_error in alpaca_rest.hpp): a buy priced at or above
// the account's open sell of the symbol (and the reverse) is refused as a potential wash trade; a
// sell larger than the long position not held by other sells is refused, and a short sale is
// refused while a buy of the symbol is open. Alpaca has no post-only order: PostOnly is sent as a
// limit order.
#include "fastmm/config/config.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/net/connection.hpp"
#include "fastmm/venues/alpaca/alpaca_md.hpp"
#include "fastmm/venues/alpaca/alpaca_rest.hpp"
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
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::alpaca {

struct AlpacaVenueConfig {
  std::string name = "alpaca";
  std::string ws_url;          // market data
  std::string ws_private_url;  // trade_updates
  std::string rest_url;        // https://paper-api.alpaca.markets
  std::string api_key;
  std::string api_secret;
  bool insecure_tls = false;
  std::string ca_file;
  net::SourceAddress source;
  bool dry_run = false;
  bool trades = true;  // subscribe to trades as well as quotes
  EncoderOptions orders{};
  bool cancel_on_order_channel_loss = true;
  bool allow_offline_reference_data = false;
  std::uint32_t stale_ms = 15'000;
  std::uint32_t dead_ms = 30'000;
  std::uint32_t ping_ms = 5'000;  // client pings: a quiet symbol sends nothing for minutes
  // New orders and replaces a minute; Alpaca counts every request of the account, 200 a minute.
  std::uint32_t requests_per_minute = 200;
  double rate_threshold = 0.8;
  std::string record_raw_dir;
  std::uint32_t http_timeout_ms = 5000;
  net::BackoffConfig backoff{};
};

class AlpacaVenue final : public Venue, private ReconcileHooks {
 public:
  AlpacaVenue(VenueId id, AlpacaVenueConfig cfg);
  ~AlpacaVenue() override;

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

  [[nodiscard]] const AlpacaVenueConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] bool fatal() const noexcept { return fatal_; }
  [[nodiscard]] std::size_t shadow_count() const noexcept { return shadows_.size(); }
  [[nodiscard]] bool order_channel_live() const noexcept { return user_conn_.is_live(); }
  [[nodiscard]] std::int64_t venue_time_ms() const noexcept;

 private:
  struct MdHandler {
    AlpacaVenue* v;
    void on_state(net::ConnState s) { v->on_md_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_md_text(t, ts); }
    void on_binary(std::span<const std::byte> b, std::int64_t ts) {
      v->on_md_text({reinterpret_cast<const char*>(b.data()), b.size()}, ts);
    }
    void on_connected_send_subscriptions() { v->on_md_open(); }
  };
  struct UserHandler {
    AlpacaVenue* v;
    void on_state(net::ConnState s) { v->on_user_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_user_text(t, ts); }
    // The paper stream sends its JSON in binary frames.
    void on_binary(std::span<const std::byte> b, std::int64_t ts) {
      v->on_user_text({reinterpret_cast<const char*>(b.data()), b.size()}, ts);
    }
    void on_connected_send_subscriptions() { v->on_user_open(); }
  };

  struct Shadow {
    InstrumentId instrument{};
    Side side = Side::Buy;
    Qty qty{};
    std::uint64_t sent_seq = 0;
    VenueOrderId venue_id{};
    ClientOrderId replace_to{};  // a replace in flight: the new client id
    bool cancel_parked = false;  // a cancel waits for the venue's order id
    bool ended = false;
  };

  void on_md_state(net::ConnState s);
  void on_md_text(std::string_view t, std::int64_t ts);
  void on_md_open();
  void on_user_state(net::ConnState s);
  void on_user_text(std::string_view t, std::int64_t ts);
  void on_user_open();
  void on_order_update(const TradeUpdate& u);

  void open_md();
  void open_user();
  [[nodiscard]] net::ConnectionConfig ws_config(const std::string& url) const;
  [[nodiscard]] std::string auth_headers(bool body) const;
  [[nodiscard]] std::string auth_frame() const;
  template <class Ring>
  void write_orders(Ring& ring);
  void send_command(const OrderCommand& cmd);
  void send_new(const OrderCommand& cmd);
  void send_replace(const OrderCommand& cmd);
  void send_cancel(ClientOrderId id, InstrumentId inst, std::string_view venue_id);
  void handle_new_reply(ClientOrderId id, const net::HttpResponse& r);
  void handle_replace_reply(ClientOrderId orig, ClientOrderId id, const net::HttpResponse& r);
  void handle_cancel_reply(ClientOrderId id, InstrumentId inst, const net::HttpResponse& r);
  void learn_venue_id(ClientOrderId id, std::string_view venue_id);
  void remember_order(std::string_view order_id, ClientOrderId cl) noexcept;
  [[nodiscard]] ClientOrderId name_of(std::string_view order_id) const noexcept;
  void end_order(ClientOrderId id) noexcept;
  void trip_venue_kill(KillReason reason);
  void apply_action(VenueAction action, int code, std::string_view msg);
  void request_clock();
  void cancel_all_async();
  [[nodiscard]] std::vector<std::string> symbols() const;
  [[nodiscard]] InstrumentId subscribed_instrument(std::string_view symbol) const noexcept;
  void refuse(const OrderCommand& cmd, RejectReason reason, std::string_view why);
  void publish_status() noexcept;
  [[nodiscard]] std::int64_t now_ns() const noexcept { return net::Reactor::now_ns(); }
  [[nodiscard]] bool private_on() const noexcept;

  // ReconcileHooks: the open orders, then the positions.
  bool fetch_snapshot(std::uint64_t generation) override;
  bool replay_executions() override;
  void shadow_ids(std::vector<SentShadow>& out) override;
  void drop_shadow(ClientOrderId id) override;
  void fetch_positions(std::uint64_t generation);
  [[nodiscard]] bool replay_ready() const noexcept;
  bool query_fills(const ReplayQuery& q);
  bool emit_replayed(const FillRow& f);
  bool lookup_order(const ReplayLookup& l);

  VenueId id_;
  AlpacaVenueConfig cfg_;
  const SymbolTable* symbols_ = nullptr;
  const InstrumentTable* instruments_ = nullptr;
  EventSink* md_sink_ = nullptr;
  EventSink* order_sink_ = nullptr;
  MsgRing* outbound_ = nullptr;
  net::Reactor* reactor_ = nullptr;

  std::unique_ptr<AlpacaMdFeed> md_feed_;
  std::unique_ptr<AlpacaOrderEncoder> encoder_;
  TradeUpdateParser updates_;
  TradeUpdate update_{};
  std::unique_ptr<RestChannel> rest_;
  MdHandler md_handler_{this};
  UserHandler user_handler_{this};
  ConnectionSlot<MdHandler> md_conn_;
  ConnectionSlot<UserHandler> user_conn_;
  RateLimiter rate_;
  OpenHashMap<ClientOrderId, Shadow, kShadowSlots> shadows_;
  ShadowOverflow shadow_overflow_;
  OpenHashMap<coinbase::OrderKey, ClientOrderId, kShadowSlots> order_names_;
  RawRecorder raw_md_;
  RawRecorder raw_user_;

  std::vector<InstrumentId> subscribed_;
  std::atomic<std::int64_t> clock_offset_ms_{0};
  std::int64_t clock_sync_ns_ = 0;
  bool clock_pending_ = false;
  bool fatal_ = false;
  bool venue_kill_sent_ = false;
  bool connected_ = false;
  bool rest_hard_stopped_ = false;
  bool user_was_live_ = false;
  SentWatermark sent_;

  ReconcileDriver reconcile_{*this, sent_};
  ReplayScheduler<FillRow> exec_replay_;
  ConnState md_state_ = ConnState::Disconnected;
  ConnState user_state_ = ConnState::Disconnected;
  net::TimerId housekeeping_timer_ = net::kInvalidTimer;
  std::uint64_t generation_ = 0;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);

  WireLatencyRecorder wire_;
  VenueStatus stats_{};
  Seqlocked<VenueStatus> published_{};
};

// [venues.<name>] -> AlpacaVenueConfig. ws_url = the market-data stream, ws_private_url = the
// trading stream, rest_url = the trading API. Throws std::invalid_argument for a bad value, for
// supports_replace with an unknown setting, and for testnet = true with the live trading host.
AlpacaVenueConfig make_alpaca_config(const VenueSection& v, bool dry_run);

}  // namespace fastmm::venues::alpaca
