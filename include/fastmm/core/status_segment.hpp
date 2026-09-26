#pragma once
// Live status for external monitors (fastmm-top). fastmm-live's control thread copies the engine's
// published stats and every venue's status into a fixed-layout snapshot a few times a second and
// writes it into a small memory-mapped file (by default under /dev/shm). Readers map the file
// read-only and retry while a write is in progress, so a monitor never blocks or slows the engine:
// the engine thread only ever touches its own Seqlocked publications. fastmm-gateway writes the
// same layout with `kind` Gateway: its venues, and in `gateway` its attachments and the account.
//
// The layout is plain old data with explicit sizes; a magic number and a version guard against
// reading a file written by an incompatible build. Bump kStatusVersion on every layout change: the
// magic and the version sit at the same offsets in every version, so a reader of another version
// refuses the file instead of misreading it.
#include "fastmm/core/latency.hpp"
#include "fastmm/core/reject_counters.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace fastmm {

inline constexpr std::uint64_t kStatusMagic = 0x315441545353464DULL;  // "MFSSTAT1" little-endian
// 2: venue_rejects and the per-reason reject counts.
// 3: kill reasons (global and per venue), per-venue kill flags and venue_kills.
// 4: p99.9 in every latency, the multicast feed block of each venue.
// 5: the latched kill state and the PnL carried over from earlier sessions.
// 6: the operator flatten's state, the instruments it has left and the orders it has sent.
// 7: quoting presence (quoting_elapsed_ns, quoting_two_sided_ns).
// 8: `kind`, and the gateway block (attachments, the account, its positions, routing counters).
inline constexpr std::uint32_t kStatusVersion = 10;
inline constexpr std::size_t kStatusMaxVenues = 8;
inline constexpr std::size_t kStatusMaxRejectReasons = 6;  // per kind (risk, venue)

enum class StatusRunState : std::uint8_t { Starting = 0, Running = 1, Stopping = 2, Stopped = 3 };
[[nodiscard]] std::string_view to_string(StatusRunState s) noexcept;
// Who writes the segment: fastmm-live (the engine's fields) or fastmm-gateway (`gateway`).
enum class StatusKind : std::uint8_t { Engine = 0, Gateway = 1 };
[[nodiscard]] std::string_view to_string(StatusKind k) noexcept;
// Venue channel states use venues::ChannelState values: 0 down, 1 connecting, 2 live, 3 stale.
[[nodiscard]] std::string_view channel_state_name(std::uint8_t s) noexcept;

struct StatusLatency {
  std::uint64_t count = 0;
  std::uint64_t p50_ns = 0;
  std::uint64_t p99_ns = 0;
  std::uint64_t p999_ns = 0;
  std::uint64_t max_ns = 0;
};

// Multicast feed of a venue (nasdaq_itch); `state` 0 for the other venues. Mirrors
// venues::VenueFeedStatus.
struct StatusFeed {
  std::uint8_t state = 0;     // venues::FeedState: 0 none, 1 down, 2 snapshot, 3 live, 4 lost
  std::uint8_t backend = 0;   // 0 kernel, 1 af_xdp, 2 dpdk
  std::uint8_t xdp_mode = 0;  // net::XdpMode: 1 zerocopy, 2 native_copy, 3 generic
  std::uint8_t pad_[5] = {};
  std::uint64_t packets = 0;
  std::uint64_t bytes = 0;
  std::uint64_t line_packets[2] = {0, 0};
  std::uint64_t line_duplicates[2] = {0, 0};
  std::int64_t line_skew_mean_ns[2] = {0, 0};
  std::int64_t line_skew_max_ns[2] = {0, 0};
  std::uint64_t gaps = 0;
  std::uint64_t recovered = 0;
  std::uint64_t unrecovered = 0;
  std::uint64_t snapshot_recoveries = 0;
  std::uint64_t recovery_overflows = 0;
  std::uint64_t reorder_high_water = 0;
  std::uint64_t requests = 0;
  std::uint64_t malformed = 0;
  std::uint64_t book_errors = 0;
  StatusLatency kernel_to_t0;
  std::uint64_t xdp_rx_dropped = 0;
  std::uint64_t xdp_rx_invalid_descs = 0;
  std::uint64_t xdp_rx_ring_full = 0;
  std::uint64_t xdp_fill_ring_empty = 0;
  std::uint64_t xdp_fallback = 0;
};

// One reason's reject count (`reason` holds a RejectReason value); count 0 marks an unused entry.
struct StatusRejectCount {
  std::uint64_t count = 0;
  std::uint8_t reason = 0;
  std::uint8_t pad_[7] = {};
};

struct StatusVenue {
  char name[24] = {};
  std::uint8_t md = 0;
  std::uint8_t user = 0;
  std::uint8_t order = 0;
  std::uint8_t killed = 0;       // this venue's kill switch is engaged
  std::uint8_t kill_reason = 0;  // KillReason it was tripped for
  std::uint8_t pad_[3] = {};
  std::uint32_t books_synced = 0;
  std::uint32_t books_total = 0;
  std::uint64_t md_messages = 0;
  std::uint64_t resyncs = 0;
  std::uint64_t orders_sent = 0;
  std::uint64_t cancels_sent = 0;
  std::uint64_t replaces_sent = 0;
  std::uint64_t order_events = 0;
  std::uint64_t reconnects = 0;
  std::uint64_t rest_errors = 0;
  std::uint64_t rate_limit_cooldowns = 0;
  std::int64_t clock_offset_ms = 0;
  StatusLatency wire_tick_to_trade;
  StatusFeed feed;
};

// ---- fastmm-gateway ---------------------------------------------------------------------------

inline constexpr std::size_t kStatusMaxAttachments = 16;  // gw::kMaxAttachments
inline constexpr std::size_t kStatusMaxPositions = 256;   // kMaxInstruments
// The gateway's refusals, counted per attachment and per venue in this order.
inline constexpr std::size_t kStatusGatewayRefusals = 7;
inline constexpr RejectReason kStatusGatewayRefusalReasons[kStatusGatewayRefusals] = {
    RejectReason::GatewayNotOwner,
    RejectReason::GatewayAccountKilled,
    RejectReason::GatewayOpenNotional,
    RejectReason::GatewayGrossNotional,
    RejectReason::GatewayNetNotional,
    RejectReason::GatewayRateLimit,
    RejectReason::GatewayFxRateUnknown};

// One attached strategy. Its instruments are the positions whose owner_epoch is its epoch.
struct StatusAttachment {
  char engine[32] = {};  // its [engine] name
  std::uint32_t pid = 0;
  std::uint32_t id = 0;  // the gateway's attachment number
  std::uint16_t epoch = 0;
  std::uint8_t blocks = 0;  // its engine blocks when idle (spin_mode = "adaptive")
  std::uint8_t pad_[5] = {};
  std::int64_t attached_ns = 0;  // wall clock
  std::uint64_t md_dropped = 0;  // market-data events its rings dropped, every venue
  std::uint64_t refused[kStatusGatewayRefusals] = {};  // its orders the gateway refused, by reason
};

// The account's position in one instrument of the gateway's table.
struct StatusPosition {
  char symbol[24] = {};
  std::uint8_t venue = 0;  // index into venues
  std::uint8_t pad_[1] = {};
  std::uint16_t owner_epoch = 0;  // the attachment that trades it, 0 for none
  std::uint32_t pad2_ = 0;
  std::int64_t qty_raw = 0;  // Qty raw (1e-8), signed
};

// One venue as the gateway routes it.
struct StatusGatewayVenue {
  std::uint64_t md_discarded = 0;     // market data with nothing attached
  std::uint64_t order_discarded = 0;  // order events with nothing attached
  std::uint64_t unrouted = 0;         // order events for no attachment
  std::uint64_t gateway_cancels = 0;  // cancels the gateway sent itself
  std::uint64_t untracked = 0;        // orders its order table had no room for
  std::uint64_t stale_replays = 0;    // replayed fills older than their owner's history
  std::uint64_t account_skipped = 0;  // replayed fills the account's seed holds
  std::uint64_t account_md_lost = 0;  // times the account's books started over
  std::uint64_t refused[kStatusGatewayRefusals] = {};
  // The account on this venue, Notional raw.
  std::int64_t realized_raw = 0;
  std::int64_t unrealized_raw = 0;
  std::int64_t fees_raw = 0;
  std::int64_t gross_raw = 0;
  std::int64_t net_raw = 0;
};

struct StatusGateway {
  std::uint32_t attachment_count = 0;
  std::uint32_t position_count = 0;
  // The account's kill switch is tripped ([gateway] max_loss or an operator's kill); its reason is
  // the snapshot's kill_reason, kill_latched says the kill file records it.
  std::uint8_t kill_active = 0;
  std::uint8_t pad_[7] = {};
  // The account over every venue, Notional raw: net_pnl = carry + realized + unrealized - fees
  // (the snapshot's pnl fields).
  std::int64_t net_pnl_raw = 0;
  std::int64_t gross_raw = 0;
  std::int64_t net_raw = 0;
  std::int64_t trip_net_raw = 0;  // the net PnL at the trip
  // [gateway] limits, raw; 0 off.
  std::int64_t max_loss_raw = 0;
  std::int64_t max_gross_raw = 0;
  std::int64_t max_net_raw = 0;
  std::int64_t max_open_notional_raw = 0;
  StatusGatewayVenue venues[kStatusMaxVenues];
  StatusAttachment attachments[kStatusMaxAttachments];
  StatusPosition positions[kStatusMaxPositions];
};

struct StatusSnapshot {
  std::uint64_t magic = kStatusMagic;
  std::uint32_t version = kStatusVersion;
  std::uint32_t pid = 0;
  std::uint64_t session_id = 0;
  std::int64_t started_ns = 0;  // wall clock
  std::int64_t updated_ns = 0;  // wall clock of the last publish
  StatusRunState state = StatusRunState::Starting;
  std::uint8_t dry_run = 0;
  std::uint8_t venue_count = 0;
  std::uint8_t kill_reason = 0;  // KillReason of the global kill switch (0 while not set)
  // A max-loss trip is latched in the durable kill state: the next start refuses to trade until an
  // operator clears it (core/session_state.hpp).
  std::uint8_t kill_latched = 0;
  // Operator flatten (fastmm-ctl flatten): FlattenState, how many instruments in its scope still
  // hold a position and how many reduce-only orders it has sent.
  std::uint8_t flatten_state = 0;
  StatusKind kind = StatusKind::Engine;
  std::uint8_t pad_[1] = {};
  std::uint32_t flatten_instruments_left = 0;
  std::uint64_t flatten_orders = 0;
  char engine_name[32] = {};
  char strategy[32] = {};
  // engine
  std::uint64_t events = 0;
  std::uint64_t book_updates = 0;
  std::uint64_t orders_sent = 0;
  std::uint64_t cancels_sent = 0;
  std::uint64_t replaces_sent = 0;
  std::uint64_t fills = 0;
  std::uint64_t risk_rejects = 0;
  std::uint64_t kills = 0;
  std::uint32_t kill_flags = 0;  // bit 0 global, bit 1 + venue id per venue
  std::uint32_t venue_kills = 0;
  std::int64_t realized_pnl_raw = 0;  // Notional raw (1e-8)
  std::int64_t unrealized_pnl_raw = 0;
  std::int64_t fees_raw = 0;
  // Net PnL of earlier sessions that [risk] max_loss is measured against on top of this one's.
  std::int64_t pnl_carry_raw = 0;
  // [risk] max_loss as the engine applies it now; zero when off (and for a gateway, whose limit is
  // gateway.max_loss_raw).
  std::int64_t max_loss_raw = 0;
  std::uint64_t venue_rejects = 0;
  // The most frequent reasons, most frequent first; the totals above include reasons that did not
  // fit (set_status_rejects).
  StatusRejectCount risk_reject_reasons[kStatusMaxRejectReasons];
  StatusRejectCount venue_reject_reasons[kStatusMaxRejectReasons];
  // Time-weighted quoting presence: elapsed since the first order rested, and how much of it had a
  // live order on both sides. A market-maker programme's rebate is measured this way.
  std::int64_t quoting_elapsed_ns = 0;
  std::int64_t quoting_two_sided_ns = 0;
  StatusLatency latency[static_cast<std::size_t>(LatencyInterval::Count)];
  StatusVenue venues[kStatusMaxVenues];
  // kind Gateway only, zero in an engine's segment. A gateway fills the header (pid, times, state,
  // dry_run, engine_name, venue_count, venues), kill_reason, kill_latched, kill_flags (bit 0 while
  // the account is killed) and the PnL fields with the account's, and leaves the rest zero.
  StatusGateway gateway;
};
static_assert(std::is_trivially_copyable_v<StatusSnapshot>);
// Every version keeps the magic and the version at these offsets (after the 8-byte sequence).
static_assert(offsetof(StatusSnapshot, magic) == 0 && offsetof(StatusSnapshot, version) == 8);

// Copies `s` into a fixed char array, truncating and always NUL-terminating.
void set_status_name(char* dst, std::size_t capacity, std::string_view s) noexcept;
template <std::size_t N>
void set_status_name(char (&dst)[N], std::string_view s) noexcept {
  set_status_name(dst, N, s);
}
[[nodiscard]] StatusLatency to_status_latency(const LatencyStats& s) noexcept;
// Fills `dst` with the kStatusMaxRejectReasons most frequent non-zero reasons of `c`.
void set_status_rejects(StatusRejectCount* dst, const RejectCounts& c);
// "MaxPosition 12, RateLimit 5" from the entries, plus "other <n>" for rejects of reasons that did
// not fit; empty if `total` is 0.
[[nodiscard]] std::string format_status_rejects(const StatusRejectCount* entries,
                                                std::uint64_t total);

// The error for a status segment of `version` when this build reads kStatusVersion.
[[nodiscard]] std::string status_version_mismatch(std::uint32_t version);

// "/dev/shm/fastmm-<engine name>.status"
[[nodiscard]] std::string default_status_path(std::string_view engine_name);
// "/dev/shm/fastmm-<engine name>.gw.status": fastmm-gateway's, apart from a strategy that runs with
// the same configuration and so the same name.
[[nodiscard]] std::string default_gateway_status_path(std::string_view engine_name);

// One writer (fastmm-live's control thread). The file is created or truncated on open and left in
// place on close, so a monitor can still show the final "stopped" snapshot.
class StatusWriter {
 public:
  StatusWriter() noexcept = default;
  StatusWriter(const StatusWriter&) = delete;
  StatusWriter& operator=(const StatusWriter&) = delete;
  ~StatusWriter();
  [[nodiscard]] bool open(const std::string& path, std::string* error);
  [[nodiscard]] bool is_open() const noexcept { return seg_ != nullptr; }
  void publish(const StatusSnapshot& s) noexcept;
  void close() noexcept;

 private:
  struct Segment;
  Segment* seg_ = nullptr;
};

class StatusReader {
 public:
  StatusReader() noexcept = default;
  StatusReader(const StatusReader&) = delete;
  StatusReader& operator=(const StatusReader&) = delete;
  ~StatusReader();
  // Fails for a file too small for this version's layout; if that file holds a status segment of
  // another version, `error` names both versions.
  [[nodiscard]] bool open(const std::string& path, std::string* error);
  [[nodiscard]] bool is_open() const noexcept { return seg_ != nullptr; }
  // A consistent copy, or false if the writer kept it busy for all retries or the file does not
  // hold a compatible snapshot.
  [[nodiscard]] bool read(StatusSnapshot& out) const noexcept;
  // The version of the status segment in the open file, 0 if it holds none (not yet published or
  // not a status file). Differs from kStatusVersion when a writer of another build owns the file.
  // After open() refused a file because it holds a segment of another version, that version.
  [[nodiscard]] std::uint32_t segment_version() const noexcept;
  void close() noexcept;

 private:
  struct Segment;
  const Segment* seg_ = nullptr;
  std::uint32_t refused_version_ = 0;
};

// The snapshot as one JSON object (fastmm-top --once --json; scripts/bench-e2e.sh reads it). Every
// object has "kind"; a gateway's carries its header fields and a "gateway" object instead of the
// engine's counters.
[[nodiscard]] std::string format_status_json(const StatusSnapshot& s);

// Human-readable dashboard frame, an engine's or a gateway's by `kind`. `now_ns` is the wall clock;
// a running process that has not published for over 3 s is shown as stale. `color` adds ANSI
// colours.
[[nodiscard]] std::string format_status(const StatusSnapshot& s, std::int64_t now_ns, bool color);

}  // namespace fastmm
