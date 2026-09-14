#pragma once
// Live status for external monitors (fastmm-top). fastmm-live's control thread copies the engine's
// published stats and every venue's status into a fixed-layout snapshot a few times a second and
// writes it into a small memory-mapped file (by default under /dev/shm). Readers map the file
// read-only and retry while a write is in progress, so a monitor never blocks or slows the engine:
// the engine thread only ever touches its own Seqlocked publications.
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
inline constexpr std::uint32_t kStatusVersion = 3;
inline constexpr std::size_t kStatusMaxVenues = 8;
inline constexpr std::size_t kStatusMaxRejectReasons = 6;  // per kind (risk, venue)

enum class StatusRunState : std::uint8_t { Starting = 0, Running = 1, Stopping = 2, Stopped = 3 };
[[nodiscard]] std::string_view to_string(StatusRunState s) noexcept;
// Venue channel states use venues::ChannelState values: 0 down, 1 connecting, 2 live, 3 stale.
[[nodiscard]] std::string_view channel_state_name(std::uint8_t s) noexcept;

struct StatusLatency {
  std::uint64_t count = 0;
  std::uint64_t p50_ns = 0;
  std::uint64_t p99_ns = 0;
  std::uint64_t max_ns = 0;
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
  std::uint8_t pad_[4] = {};
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
  std::uint64_t venue_rejects = 0;
  // The most frequent reasons, most frequent first; the totals above include reasons that did not
  // fit (set_status_rejects).
  StatusRejectCount risk_reject_reasons[kStatusMaxRejectReasons];
  StatusRejectCount venue_reject_reasons[kStatusMaxRejectReasons];
  StatusLatency latency[static_cast<std::size_t>(LatencyInterval::Count)];
  StatusVenue venues[kStatusMaxVenues];
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

// Human-readable dashboard frame. `now_ns` is the wall clock; a running engine that has not
// published for over 3 s is shown as stale. `color` adds ANSI colours.
[[nodiscard]] std::string format_status(const StatusSnapshot& s, std::int64_t now_ns, bool color);

}  // namespace fastmm
