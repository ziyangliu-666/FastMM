#pragma once
// HyperliquidVenue: the Hyperliquid connector for perpetuals, the main dex and the builder-deployed
// (HIP-3) dexes such as xyz's US equities ("xyz:NVDA").
//
// Channels on one reactor thread (WebSocket and info/exchange docs, read 2026-10-12):
//   md       ws_url (wss://api.hyperliquid.xyz/ws)  l2Book, bbo, trades per coin (HlMdFeed)
//   private  the same URL, a second connection      orderUpdates, userFills, userFundings of the
//                                                   account address (no auth: they are public)
//   trade    the same URL, a third connection       {"method":"post"} actions: order, cancel,
//                                                   cancelByCloid, batchModify; REST fallback
//   rest     rest_url (https://api.hyperliquid.xyz) POST /info (meta, open orders, positions,
//                                                   balances, fills, the address budget),
//                                                   POST /exchange (the fallback, scheduleCancel)
// Every connection sends {"method":"ping"} every ping_interval_ms: the venue closes one that has
// sent nothing for 60 s.
//
// Keys: `api_key` is the account (master) address, `api_secret` the signing key: an agent wallet
// the account approved (load_reference_data() checks extraAgents), or the account's own. Orders
// are signed by the key; positions, orders and fills are queried by the account. `vault_address`
// trades for a vault or sub-account the account controls.
//
// Instruments: the symbol is the venue's coin, with its dex prefix ("xyz:NVDA"), or without it
// when `dex` names the dex. load_reference_data() reads perpDexs and metaAndAssetCtxs of each dex
// the instruments use: asset id (the index in the main dex's meta, 100000 + dex index * 10000 +
// the index for a builder dex), szDecimals (lot = 10^-szDecimals), the tick at the current mark
// (5 significant figures, at most 6 - szDecimals decimals; the encoder rounds every price onto the
// grid, never more aggressively), min notional 10 USDC, quote USDC.
//
// Order entry: the orders the engine hands over in one drain go out as few actions as possible:
// news as one order action (at most 40 orders), cancels as one cancel (by oid once a reply or the
// stream gave it, else cancelByCloid), replaces as one batchModify; each a post with its own id.
// The reply answers every order of the action by position: resting/filled is the ack, an error
// text the reject (hl_error_map.hpp). The orderUpdates stream says the same again (open, canceled,
// *Rejected, *Canceled); whichever comes first goes to the engine, once. Post-only is "Alo",
// IOC / FOK / market "Ioc", the rest "Gtc"; cloid = 0x666d000000000000 + the client order id.
// A replace (`supports_replace`) is a batchModify, which the venue carries out as a new order:
// the engine gets the new id's ack without kAmendedInPlace, and the old order's "canceled" is
// swallowed.
//
// Limits: each new order and modify counts against the address's budget (1 request per USDC
// traded since inception, plus 10000; growth-mode volume counts 10 %; cancels have a larger
// limit). The connector reads it (userRateLimit) at start and every rate_limit_refresh_s, counts
// what it sends in between, and refuses a new order or modify when no more than
// `address_reserve` would remain (VenueRateLimit "local rate limit: address budget", counted with
// the day-long refusals). Posts are capped at `posts_per_minute` (the venue allows 2000 messages a
// minute over every connection) and orders at `orders_per_second`.
//
// Reconciliation: frontendOpenOrders and clearinghouseState of each dex for the snapshot; the
// fill replay reads userFillsByTime. Balances: spotClearinghouseState (a unified account keeps
// its perp collateral in its spot USDC) plus the dexes' accountValue.
//
// Latency: every action's send time is kept; the post reply's round trip and, for orders, the
// block time of their "open" on the stream minus the send time are summarised in the log once a
// minute (latency_log_s).
#include "fastmm/config/config.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/net/connection.hpp"
#include "fastmm/venues/connection_slot.hpp"
#include "fastmm/venues/dead_mans_switch.hpp"
#include "fastmm/venues/error_action.hpp"
#include "fastmm/venues/hyperliquid/hl_crypto.hpp"
#include "fastmm/venues/hyperliquid/hl_md_feed.hpp"
#include "fastmm/venues/hyperliquid/hl_order_encoder.hpp"
#include "fastmm/venues/hyperliquid/hl_private_parser.hpp"
#include "fastmm/venues/hyperliquid/hl_response.hpp"
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

namespace fastmm::venues::hyperliquid {

struct HyperliquidVenueConfig {
  std::string name = "hyperliquid";
  std::string ws_url;    // wss://api.hyperliquid.xyz/ws
  std::string rest_url;  // https://api.hyperliquid.xyz
  bool mainnet = true;
  std::string account_address;  // the master account ("0x" + 40 hex)
  std::string private_key;      // the signing key (agent wallet or the account's)
  std::string vault_address;    // trade for a vault / sub-account; empty: none
  std::string dex;              // the dex unprefixed symbols belong to; empty: none
  bool insecure_tls = false;
  std::string ca_file;
  net::SourceAddress source;
  bool dry_run = false;
  bool ws_order_api = true;
  MdChannels channels{};
  bool emit_ack_from_response = true;
  bool cancel_on_order_channel_loss = true;
  bool allow_offline_reference_data = false;
  bool supports_replace = false;
  std::uint32_t stale_ms = 5000;
  std::uint32_t dead_ms = 60'000;
  std::uint32_t ping_interval_ms = 30'000;
  std::uint32_t orders_per_second = 20;
  std::uint32_t posts_per_minute = 1800;
  std::int64_t address_reserve = 200;
  std::int64_t rate_limit_refresh_s = 60;
  // scheduleCancel window in seconds (at least 5), refreshed at a third of it; 0 disables it.
  std::int64_t dead_mans_switch_s = 0;
  std::int64_t latency_log_s = 60;
  std::string record_raw_dir;
  std::uint32_t http_timeout_ms = 5000;
  net::BackoffConfig backoff{};
};

// The account's request budget (userRateLimit) and what was sent since it was read.
struct AddressBudget {
  std::int64_t cap = 0;   // nRequestsCap + nRequestsSurplus (reserved with reserveRequestWeight)
  std::int64_t used = 0;  // nRequestsUsed when read, plus the requests counted since
  bool known = false;
  [[nodiscard]] std::int64_t remaining() const noexcept { return cap - used; }
};

class HyperliquidVenue final : public Venue, private ReconcileHooks {
 public:
  HyperliquidVenue(VenueId id, HyperliquidVenueConfig cfg);
  ~HyperliquidVenue() override;

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
  void request_balances() override { reconcile_.request_balances(); }
  bool cancel_all() override;
  [[nodiscard]] VenueStatus status() const noexcept override;

  [[nodiscard]] const HyperliquidVenueConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] bool fatal() const noexcept { return fatal_; }
  [[nodiscard]] std::size_t shadow_count() const noexcept { return shadows_.size(); }
  [[nodiscard]] bool order_channel_live() const noexcept { return trade_conn_.is_live(); }
  [[nodiscard]] const AddressBudget& address_budget() const noexcept { return budget_; }
  [[nodiscard]] const HlMdFeed* md_feed() const noexcept { return md_feed_.get(); }
  // The asset id and szDecimals load_reference_data() found for an instrument.
  [[nodiscard]] std::uint32_t asset_of(InstrumentId id) const noexcept {
    return id.value < kMaxInstruments ? assets_[id.value].asset : 0;
  }
  [[nodiscard]] int sz_decimals_of(InstrumentId id) const noexcept {
    return id.value < kMaxInstruments ? assets_[id.value].sz_decimals : 0;
  }
  [[nodiscard]] std::string_view coin_of(InstrumentId id) const noexcept {
    return id.value < kMaxInstruments ? std::string_view(coins_[id.value]) : std::string_view{};
  }

  // For tests and the probe tool: the asset table without the network.
  void set_asset(InstrumentId id, std::string coin, std::uint32_t asset, int sz_decimals);
  void set_address_budget(std::int64_t used, std::int64_t cap) noexcept {
    budget_.used = used;
    budget_.cap = cap;
    budget_.known = true;
  }

 private:
  // Per working order: what a cancel, a modify and the fills need that the engine message does
  // not carry.
  struct Shadow {
    InstrumentId instrument{};
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::Gtc;
    ClientOrderId replaces{};  // a replace: the order it modifies
    std::uint64_t sent_seq = 0;
    std::uint64_t oid = 0;  // the venue's id, once a reply or the stream named it
    std::int64_t sent_wall_ns = 0;
    Qty orig_qty{};
    Qty cum_qty{};
    bool acked = false;     // an ack went to the engine
    bool answered = false;  // the reply or the stream decided the new order or modify
    bool retired = false;   // modified away: its own terminal update is not forwarded
  };
  struct Entry {
    OrderCommandKind kind = OrderCommandKind::New;
    InstrumentId instrument{};
    ClientOrderId cl_ord_id{};
    ClientOrderId orig_cl_ord_id{};
  };
  // One action in flight: its entries in the order of the action's statuses.
  struct Pending {
    std::uint64_t id = 0;  // 0: free slot
    ActionKind kind = ActionKind::Order;
    std::uint32_t count = 0;
    std::int64_t sent_ns = 0;
    std::array<Entry, kMaxBatch> entries{};
  };
  static constexpr std::size_t kPendingSlots = 256;

  struct MdHandler {
    HyperliquidVenue* v;
    void on_state(net::ConnState s) { v->on_md_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_md_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_md_open(); }
  };
  struct PrivateHandler {
    HyperliquidVenue* v;
    void on_state(net::ConnState s) { v->on_private_state(s); }
    void on_text(std::string_view t, std::int64_t ts) { v->on_private_text(t, ts); }
    void on_binary(std::span<const std::byte>, std::int64_t) {}
    void on_connected_send_subscriptions() { v->on_private_open(); }
  };
  struct TradeHandler {
    HyperliquidVenue* v;
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
    static_cast<HyperliquidVenue*>(ctx)->request_resubscribe(id);
  }

  // ---- order entry ---------------------------------------------------------------------------
  template <class Ring>
  void write_orders(Ring& ring);
  void drain_outbound();
  void stage(const OrderCommand& cmd);
  bool flush_staged();
  void send_action(ActionKind kind, std::span<const Entry> entries);
  void refuse(const OrderCommand& cmd, RejectReason reason, std::string_view why);
  void reject_entries(std::span<const Entry> entries, RejectReason reason, std::string_view why);
  void handle_reply(const Pending& p, const PostReply& r);
  void apply_action(VenueAction action, std::string_view why);
  void trip_venue_kill(KillReason reason);
  Pending* new_pending(std::uint64_t id);
  Pending* find_pending(std::uint64_t id);
  void forget_order(ClientOrderId id) noexcept;
  void complete_fill(OrderFillMsg& f) noexcept;
  [[nodiscard]] bool wire_of(InstrumentId inst,
                             Side side,
                             OrderType type,
                             TimeInForce tif,
                             bool reduce_only,
                             Price px,
                             Qty qty,
                             ClientOrderId id,
                             OrderWire& out) const noexcept;
  [[nodiscard]] bool budget_allows() const noexcept;

  // ---- control path ----------------------------------------------------------------------------
  // POST /info with `body`; the callback gets the reply (or an empty body and false on error).
  bool info(const std::string& body, std::function<void(bool ok, std::string_view body)> done);
  void refresh_rate_limit();
  bool fetch_snapshot(std::uint64_t generation) override;
  bool fetch_orders(std::uint64_t generation, std::size_t dex_index);
  bool fetch_positions(std::uint64_t generation, std::size_t dex_index);
  bool fetch_balances(std::uint64_t generation) override;
  bool replay_executions() override;
  void shadow_ids(std::vector<SentShadow>& out) override;
  void drop_shadow(ClientOrderId id) override;
  bool query_fills(const ReplayQuery& q);
  bool emit_fill_row(const FillRow& f);
  void send_schedule_cancel(std::int64_t time_ms);
  void cancel_all_async();
  void publish_status() noexcept;
  void log_latency(std::int64_t now_ns);
  [[nodiscard]] std::int64_t now_ns() const noexcept { return net::Reactor::now_ns(); }
  [[nodiscard]] static std::int64_t wall_ms() noexcept { return wall_now().ns / 1'000'000; }
  [[nodiscard]] InstrumentId instrument_of(std::string_view coin) const noexcept;

  VenueId id_;
  HyperliquidVenueConfig cfg_;
  Signer signer_;
  ActionSigner action_signer_;
  const SymbolTable* symbols_ = nullptr;
  const InstrumentTable* instruments_ = nullptr;
  EventSink* md_sink_ = nullptr;
  EventSink* order_sink_ = nullptr;
  MsgRing* outbound_ = nullptr;
  net::Reactor* reactor_ = nullptr;

  struct AssetSpec {
    std::uint32_t asset = 0;
    int sz_decimals = 0;
    bool known = false;
  };
  std::array<AssetSpec, kMaxInstruments> assets_{};
  std::array<std::string, kMaxInstruments> coins_{};
  std::vector<std::string> dexes_;  // the dexes of the subscribed instruments ("" = main)

  std::unique_ptr<HlMdFeed> md_feed_;
  std::unique_ptr<HlPrivateParser> private_parser_;
  std::unique_ptr<HlResponseDecoder> decoder_;
  std::unique_ptr<RestChannel> rest_;
  MdHandler md_handler_{this};
  PrivateHandler private_handler_{this};
  TradeHandler trade_handler_{this};
  ConnectionSlot<MdHandler> md_conn_;
  ConnectionSlot<PrivateHandler> private_conn_;
  ConnectionSlot<TradeHandler> trade_conn_;
  RateLimiter rate_;
  AddressBudget budget_;
  OpenHashMap<ClientOrderId, Shadow, kShadowSlots> shadows_;
  ShadowOverflow shadow_overflow_;
  std::array<Pending, kPendingSlots> pending_{};
  std::uint64_t next_post_id_ = 0;
  std::unique_ptr<ActionBuilder> action_;
  // The drain's orders, staged until the batch is flushed.
  std::array<OrderWire, WireLatencyRecorder::kMaxBatch> stage_orders_{};
  std::array<Entry, WireLatencyRecorder::kMaxBatch> stage_order_entries_{};
  std::size_t n_orders_ = 0;
  std::array<CancelWire, WireLatencyRecorder::kMaxBatch> stage_cancels_{};
  std::array<Entry, WireLatencyRecorder::kMaxBatch> stage_cancel_entries_{};
  std::size_t n_cancels_ = 0;
  std::array<CancelCloidWire, WireLatencyRecorder::kMaxBatch> stage_cloid_cancels_{};
  std::array<Entry, WireLatencyRecorder::kMaxBatch> stage_cloid_entries_{};
  std::size_t n_cloid_cancels_ = 0;
  std::array<ModifyWire, WireLatencyRecorder::kMaxBatch> stage_modifies_{};
  std::array<Entry, WireLatencyRecorder::kMaxBatch> stage_modify_entries_{};
  std::size_t n_modifies_ = 0;
  // The entries of the posts written into the corked trade connection, for a failed uncork.
  std::array<Entry, WireLatencyRecorder::kMaxBatch> corked_{};
  std::size_t n_corked_ = 0;
  std::array<std::uint64_t, WireLatencyRecorder::kMaxBatch> corked_posts_{};
  std::size_t n_corked_posts_ = 0;
  std::unique_ptr<char[]> request_buf_;
  alignas(64) std::byte scratch_[kDecoderScratchBytes];
  RawRecorder raw_md_;
  RawRecorder raw_private_;
  RawRecorder raw_trade_;

  std::vector<InstrumentId> subscribed_;
  std::uint32_t private_subscribed_ = 0;
  std::int64_t last_ping_ns_ = 0;
  std::int64_t last_rate_refresh_ns_ = 0;
  bool rate_refresh_pending_ = false;
  bool fatal_ = false;
  bool venue_kill_sent_ = false;
  bool connected_ = false;
  bool private_was_live_ = false;
  bool trade_was_live_ = false;
  bool refused_account_settings_ = false;
  SentWatermark sent_;
  BatchedOrders batch_;
  ReconcileDriver reconcile_{*this, sent_};
  CountdownDriver dms_;
  // The venue refused scheduleCancel ("Cannot set scheduled cancel time ...": it needs the
  // account to have traded first): logged once, and the session goes on without the switch.
  bool dms_refused_ = false;

  ReplayScheduler<FillRow> fill_replay_;

  // Latency samples since the last summary, ms: post round trips, and block time of an order's
  // "open" minus its send time.
  std::vector<std::int64_t> post_rtt_ms_;
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

// [venues.<name>] -> HyperliquidVenueConfig. Extra keys: vault_address, dex,
// order_api ("ws"|"rest"), l2_book, bbo, trades, stale_ms, dead_ms, ping_interval_ms,
// orders_per_second, posts_per_minute, address_reserve, rate_limit_refresh_s,
// emit_ack_from_response, cancel_on_order_channel_loss, allow_offline_reference_data,
// dead_mans_switch_s, latency_log_s.
HyperliquidVenueConfig make_hyperliquid_config(const VenueSection& section, bool dry_run);

}  // namespace fastmm::venues::hyperliquid
