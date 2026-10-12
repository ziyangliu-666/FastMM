#pragma once
// LighterVenue: the Lighter (zkLighter) connector for perpetuals.
//
// Channels on one reactor thread (the WebSocket and REST references, read 2026-10-13):
//   md       ws_url (wss://mainnet.zklighter.elliot.ai/stream)  order_book, ticker, trade per
//                                                    market (LighterMdFeed)
//   private  the same URL, a second connection       account_all_orders (auth token) and
//                                                    account_all_trades of the account
//   trade    the same URL, a third connection        {"type":"jsonapi/sendtx"} per transaction,
//                                                    answered under its id; REST fallback
//   rest     rest_url (https://mainnet.zklighter.elliot.ai)  orderBookDetails, nextNonce,
//                                                    apikeys, account, accountActiveOrders,
//                                                    sendTx
// Every connection sends {"type":"ping"} every ping_interval_ms: the venue closes one that sent
// nothing for 2 minutes.
//
// Keys: `api_key` is the account index (a sub-account's works the same), `api_secret` the API
// key's private key (80 hex digits), `api_key_index` the slot it was registered in (4 to 254;
// tools/lighter_api_key.py makes and registers one). load_reference_data() checks the venue lists
// this key's public key in that slot and reads the slot's next nonce.
//
// Instruments: the symbol is the market's ("NVDA", "BTC"). orderBookDetails gives the market
// index, the size and price decimals (lot = 10^-size_decimals, tick = 10^-price_decimals), the
// minimum base amount and the minimum quote amount (min notional); quote USDC.
//
// Order entry: one signed transaction per order command, each its own sendtx frame (the drain's
// frames leave in one write). The client order index is the engine's client order id itself (48
// bits, as the venue allows), so the account's orders and fills name it; the account should
// trade through FastMM only. Post-only is TimeInForce 2, IOC / market TimeInForce 0 with no
// expiry, the rest good-till-time with an expiry of order_expiry_s. A replace
// (`supports_replace`) is a ModifyOrder of the same order, applied in place: the engine gets an
// ack with kAmendedInPlace and the order's later events under the new id. A cancel names the
// client order index.
//
// Nonces: every transaction takes the API key's next nonce; with skip_nonce (default) it carries
// the SkipNonce attribute, so a transaction the venue refuses does not fail the ones sent after
// it. A reply that names the nonce reads nextNonce again.
//
// The reply (code 200) means accepted, not executed: the ack is the order's "open" on
// account_all_orders (or the reply, with emit_ack_from_response). An order the venue ends before
// it rests (canceled-post-only, canceled-margin-not-allowed, an IOC that found nothing) is a
// reject; one it ends later an expiry. Fills come from account_all_trades with the fee from the
// trade's rate; a cancel or expiry waits until the fills it counts have arrived (at most
// terminal_wait_ms).
//
// Limits: sendTx is capped at tx_per_minute (the venue allows 4000 a minute on a Premium account
// with no LIT staked, 60 on a Standard one) and orders at orders_per_second. On Premium each
// CreateOrder, ModifyOrder and CancelAll spends one unit of the account's volume quota (1 per 2
// USDC traded, 1000 to start, one free transaction every 15 s): the replies say what is left, and
// a new order or modify is refused here when no more than quota_reserve would remain.
//
// Reconciliation: accountActiveOrders (authenticated) for the open orders, account for the
// positions and the USDC collateral.
//
// Dead man's switch: a scheduled CancelAll at now + dead_mans_switch_s (at least 310: the venue
// wants the time 5 minutes ahead at the least), refreshed at a third of it over the trade
// connection, aborted at a clean stop.
#include "fastmm/config/config.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/net/connection.hpp"
#include "fastmm/venues/connection_slot.hpp"
#include "fastmm/venues/dead_mans_switch.hpp"
#include "fastmm/venues/lighter/lighter_crypto.hpp"
#include "fastmm/venues/lighter/lighter_md_feed.hpp"
#include "fastmm/venues/lighter/lighter_private.hpp"
#include "fastmm/venues/lighter/lighter_tx.hpp"
#include "fastmm/venues/order_commands.hpp"
#include "fastmm/venues/rate_limiter.hpp"
#include "fastmm/venues/raw_recorder.hpp"
#include "fastmm/venues/reconcile_driver.hpp"
#include "fastmm/venues/rest_channel.hpp"
#include "fastmm/venues/venue.hpp"
#include "fastmm/venues/wire_latency.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::lighter {

struct LighterVenueConfig {
  std::string name = "lighter";
  std::string ws_url;    // wss://mainnet.zklighter.elliot.ai/stream
  std::string rest_url;  // https://mainnet.zklighter.elliot.ai
  std::uint32_t chain_id = kMainnetChainId;
  std::int64_t account_index = -1;
  std::int64_t api_key_index = -1;
  std::string private_key;  // the API key's, 80 hex digits
  bool insecure_tls = false;
  std::string ca_file;
  net::SourceAddress source;
  bool dry_run = false;
  bool ws_order_api = true;
  MdChannels channels{};
  bool emit_ack_from_response = false;
  bool cancel_on_order_channel_loss = true;
  bool allow_offline_reference_data = false;
  bool supports_replace = false;
  bool skip_nonce = true;
  std::uint32_t stale_ms = 5000;
  std::uint32_t dead_ms = 60'000;
  std::uint32_t ping_interval_ms = 30'000;
  std::uint32_t orders_per_second = 20;
  std::uint32_t tx_per_minute = 3600;
  std::int64_t quota_reserve = 50;
  std::int64_t order_expiry_s = kDefaultOrderExpiryMs / 1000;
  std::int64_t auth_ttl_s = 7LL * 3600;  // the venue allows 8 hours
  std::int64_t terminal_wait_ms = 2000;
  // Scheduled CancelAll window in seconds (at least 310), refreshed at a third of it; 0 disables.
  std::int64_t dead_mans_switch_s = 0;
  std::int64_t latency_log_s = 60;
  std::string record_raw_dir;
  std::uint32_t http_timeout_ms = 5000;
  net::BackoffConfig backoff{};
};

class LighterVenue final : public Venue, private ReconcileHooks {
 public:
  LighterVenue(VenueId id, LighterVenueConfig cfg);
  ~LighterVenue() override;

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
  void request_balances() override { reconcile_.request_balances(); }
  bool cancel_all() override;
  [[nodiscard]] VenueStatus status() const noexcept override;

  [[nodiscard]] const LighterVenueConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] bool fatal() const noexcept { return fatal_; }
  [[nodiscard]] std::size_t shadow_count() const noexcept { return shadows_.size(); }
  [[nodiscard]] bool order_channel_live() const noexcept { return trade_conn_.is_live(); }
  [[nodiscard]] bool private_channel_live() const noexcept { return private_conn_.is_live(); }
  [[nodiscard]] std::int64_t next_nonce() const noexcept { return nonce_; }
  [[nodiscard]] std::int64_t quota_remaining() const noexcept { return quota_; }
  [[nodiscard]] const Signer& signer() const noexcept { return signer_; }
  [[nodiscard]] std::uint32_t market_of(InstrumentId id) const noexcept {
    return id.value < kMaxInstruments ? markets_[id.value].market : 0;
  }
  [[nodiscard]] int size_decimals_of(InstrumentId id) const noexcept {
    return id.value < kMaxInstruments ? markets_[id.value].size_decimals : 0;
  }
  [[nodiscard]] int price_decimals_of(InstrumentId id) const noexcept {
    return id.value < kMaxInstruments ? markets_[id.value].price_decimals : 0;
  }
  [[nodiscard]] const LighterMdFeed* md_feed() const noexcept { return md_feed_.get(); }

  // For tests and tools: the market table and the counters without the network.
  void set_market(InstrumentId id, std::uint32_t market, int size_decimals, int price_decimals);
  void set_nonce(std::int64_t next) noexcept { nonce_ = next; }
  void set_quota(std::int64_t remaining) noexcept { quota_ = remaining; }

  // Price and size as the venue's integers: the price rounded onto the grid never more
  // aggressively (bids down, asks up), the size down. 0 when it does not fit.
  [[nodiscard]] static std::uint32_t price_units(Price px, int price_decimals, bool down) noexcept;
  [[nodiscard]] static std::int64_t size_units(Qty q, int size_decimals) noexcept;

 private:
  struct MarketSpec {
    std::uint32_t market = 0;
    int size_decimals = 0;
    int price_decimals = 0;
    bool known = false;
  };
  enum class Terminal : std::uint8_t { None, CancelAck, Expired, Reject };
  // Per working order: what a cancel, a modify and the fills need that the engine message does
  // not carry.
  struct Shadow {
    InstrumentId instrument{};
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::Gtc;
    ClientOrderId replaces{};       // a replace: the order it modifies
    std::int64_t client_index = 0;  // the venue's client order index (the first id of the chain)
    std::int64_t order_index = 0;   // the venue's, once the stream named it
    std::uint64_t sent_seq = 0;
    std::int64_t sent_wall_ns = 0;
    std::int64_t accepted_ns = 0;  // the reply accepted it (steady)
    Qty orig_qty{};
    Qty cum_qty{};
    // A cancel or end the stream reported, held until the fills it counts are in.
    Terminal terminal = Terminal::None;
    RejectReason terminal_reason = RejectReason::VenueReject;
    Qty terminal_cum{};
    std::int64_t terminal_ns = 0;
    std::int64_t terminal_venue_us = 0;
    bool acked = false;     // an ack went to the engine
    bool answered = false;  // the reply or the stream decided it
    bool seen = false;      // the stream named it
    bool late_logged = false;
  };
  struct Entry {
    OrderCommandKind kind = OrderCommandKind::New;
    InstrumentId instrument{};
    ClientOrderId cl_ord_id{};
    ClientOrderId orig_cl_ord_id{};
    std::int64_t nonce = 0;
    bool quota = false;  // spends volume quota
  };
  // One transaction in flight.
  struct Pending {
    std::uint64_t id = 0;  // 0: free slot
    std::int64_t sent_ns = 0;
    Entry entry{};
    bool control = false;  // a scheduled CancelAll (the dead man's switch)
    std::uint32_t round = 0;
  };
  static constexpr std::size_t kPendingSlots = 1024;

  struct MdHandler {
    LighterVenue* v;
    void on_state(net::ConnState s) { v->on_md_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_md_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_md_open(); }
  };
  struct PrivateHandler {
    LighterVenue* v;
    void on_state(net::ConnState s) { v->on_private_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_private_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_private_open(); }
  };
  struct TradeHandler {
    LighterVenue* v;
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
  net::ConnectionConfig ws_config(bool manual_subscribe) const;
  void request_resubscribe(InstrumentId id);
  static void resubscribe_requester(void* ctx, InstrumentId id) noexcept {
    static_cast<LighterVenue*>(ctx)->request_resubscribe(id);
  }

  // ---- account stream ----------------------------------------------------------------------
  void on_order_row(const OrderRow& row);
  void on_fill_row(const FillRow& row);
  [[nodiscard]] ClientOrderId current_id(std::int64_t client_index) const noexcept;
  void ack(ClientOrderId id, Shadow& s, Timestamp venue_ts);
  void flush_terminal(ClientOrderId id, Shadow& s);
  void forget_order(ClientOrderId id) noexcept;
  void push_order_event(EventHeader& h);

  // ---- order entry ---------------------------------------------------------------------------
  template <class Ring>
  void write_orders(Ring& ring);
  void drain_outbound();
  void stage(const OrderCommand& cmd);
  bool send_tx(std::uint8_t tx_type, std::string_view tx_info, const Entry& e);
  void refuse(const OrderCommand& cmd, RejectReason reason, std::string_view why);
  void reject_entry(const Entry& e, RejectReason reason, std::string_view why);
  void handle_reply(const Pending& p, const TxReply& r);
  void handle_control_reply(const Pending& p, const TxReply& r);
  void trip_venue_kill(KillReason reason);
  void apply_fatal(std::string_view why);
  Pending* new_pending(std::uint64_t id);
  Pending* find_pending(std::uint64_t id);
  [[nodiscard]] TxHeader tx_header() noexcept;
  [[nodiscard]] bool quota_allows() const noexcept;

  // ---- control path ----------------------------------------------------------------------------
  bool get(const std::string& target,
           bool auth,
           std::function<void(bool ok, std::string_view body)> done);
  [[nodiscard]] const std::string& auth() noexcept;
  void refresh_nonce();
  bool fetch_snapshot(std::uint64_t generation) override;
  bool fetch_positions(std::uint64_t generation);
  bool fetch_balances(std::uint64_t generation) override;
  void shadow_ids(std::vector<SentShadow>& out) override;
  void drop_shadow(ClientOrderId id) override;
  // A CancelAll signed now: `tif` kCancelAllNow, kCancelAllScheduled (at time_ms) or
  // kCancelAllAbort. The tx_info, or empty.
  std::string signed_cancel_all(std::uint8_t tif, std::int64_t time_ms);
  void send_schedule_cancel(std::int64_t time_ms);
  void cancel_all_async();
  void publish_status() noexcept;
  void log_latency(std::int64_t now_ns);
  [[nodiscard]] std::int64_t now_ns() const noexcept { return net::Reactor::now_ns(); }
  [[nodiscard]] static std::int64_t wall_ms() noexcept { return wall_now().ns / 1'000'000; }
  [[nodiscard]] InstrumentId instrument_of_market(std::int64_t market) const noexcept;
  [[nodiscard]] bool keys() const noexcept {
    return !cfg_.dry_run && signer_.usable() && cfg_.account_index >= 0 && cfg_.api_key_index >= 0;
  }

  VenueId id_;
  LighterVenueConfig cfg_;
  Signer signer_;
  const SymbolTable* symbols_ = nullptr;
  const InstrumentTable* instruments_ = nullptr;
  EventSink* md_sink_ = nullptr;
  EventSink* order_sink_ = nullptr;
  MsgRing* outbound_ = nullptr;
  net::Reactor* reactor_ = nullptr;

  std::array<MarketSpec, kMaxInstruments> markets_{};
  std::array<std::string, kMaxInstruments> symbols_by_id_{};
  MarketMap market_map_;

  std::unique_ptr<LighterMdFeed> md_feed_;
  std::unique_ptr<LighterPrivateParser> private_parser_;
  std::unique_ptr<LighterPrivateParser> trade_parser_;
  std::unique_ptr<RestChannel> rest_;
  MdHandler md_handler_{this};
  PrivateHandler private_handler_{this};
  TradeHandler trade_handler_{this};
  ConnectionSlot<MdHandler> md_conn_;
  ConnectionSlot<PrivateHandler> private_conn_;
  ConnectionSlot<TradeHandler> trade_conn_;
  RateLimiter rate_;
  OpenHashMap<ClientOrderId, Shadow, kShadowSlots> shadows_;
  // The venue's client order index of a modified order -> the engine's id it now goes by.
  OpenHashMap<ClientOrderId, ClientOrderId, kShadowSlots> aliases_;
  ShadowOverflow shadow_overflow_;
  std::array<Pending, kPendingSlots> pending_{};
  std::uint64_t next_tx_id_ = 0;
  std::int64_t nonce_ = -1;  // the next nonce; -1 unknown
  std::int64_t quota_ = -1;  // volume quota left; -1 unknown
  bool nonce_refresh_pending_ = false;
  std::string auth_;
  std::int64_t auth_deadline_s_ = 0;
  std::unique_ptr<char[]> request_buf_;
  std::unique_ptr<char[]> info_buf_;
  // The entries of the frames written into the corked trade connection, for a failed uncork.
  std::array<Entry, WireLatencyRecorder::kMaxBatch> corked_{};
  std::array<std::uint64_t, WireLatencyRecorder::kMaxBatch> corked_ids_{};
  std::size_t n_corked_ = 0;
  RawRecorder raw_md_;
  RawRecorder raw_private_;
  RawRecorder raw_trade_;

  std::vector<InstrumentId> subscribed_;
  std::uint32_t private_subscribed_ = 0;
  std::int64_t private_opened_ns_ = 0;
  std::int64_t last_ping_ns_ = 0;
  bool fatal_ = false;
  bool venue_kill_sent_ = false;
  bool connected_ = false;
  bool private_was_live_ = false;
  bool trade_was_live_ = false;
  bool refused_account_settings_ = false;
  SentWatermark sent_;
  ReconcileDriver reconcile_{*this, sent_};
  CountdownDriver dms_;
  bool dms_refused_ = false;

  std::vector<std::int64_t> reply_rtt_ms_;
  std::vector<std::int64_t> open_lag_ms_;
  std::int64_t last_latency_log_ns_ = 0;

  ConnState md_state_ = ConnState::Disconnected;
  ConnState private_state_ = ConnState::Disconnected;
  ConnState trade_state_ = ConnState::Disconnected;
  net::TimerId housekeeping_timer_ = net::kInvalidTimer;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);

  WireLatencyRecorder wire_;
  VenueStatus stats_{};
  Seqlocked<VenueStatus> published_{};
};

// [venues.<name>] -> LighterVenueConfig. Extra keys: api_key_index, order_api ("ws"|"rest"),
// order_book, ticker, trades, skip_nonce, stale_ms, dead_ms, ping_interval_ms, orders_per_second,
// tx_per_minute, quota_reserve, order_expiry_s, auth_ttl_s, terminal_wait_ms,
// emit_ack_from_response, cancel_on_order_channel_loss, allow_offline_reference_data,
// dead_mans_switch_s, latency_log_s.
LighterVenueConfig make_lighter_config(const VenueSection& section, bool dry_run);

}  // namespace fastmm::venues::lighter
