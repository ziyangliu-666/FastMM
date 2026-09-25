#include "fastmm/core/status_segment.hpp"

#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"

#include <fmt/format.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace fastmm {

namespace {

// File layout: a sequence counter (odd while a write is in progress) followed by the snapshot.
struct SegmentLayout {
  std::atomic<std::uint64_t> seq{0};
  StatusSnapshot data;
};

constexpr std::size_t kSegmentBytes = sizeof(SegmentLayout);
// The part every version shares: sequence, magic, version.
constexpr std::size_t kHeaderBytes =
    sizeof(std::uint64_t) + offsetof(StatusSnapshot, version) + sizeof(std::uint32_t);

std::string errno_text(const char* what) {
  return fmt::format("{}: {}", what, std::strerror(errno));
}

std::string fmt_ns(std::uint64_t ns) {
  if (ns == 0) return "-";
  if (ns < 1'000) return fmt::format("{} ns", ns);
  if (ns < 1'000'000) return fmt::format("{:.1f} us", static_cast<double>(ns) / 1e3);
  if (ns < 1'000'000'000) return fmt::format("{:.2f} ms", static_cast<double>(ns) / 1e6);
  return fmt::format("{:.2f} s", static_cast<double>(ns) / 1e9);
}

std::string fmt_duration_s(std::int64_t ns) {
  const std::int64_t s = std::max<std::int64_t>(ns, 0) / 1'000'000'000;
  if (s < 60) return fmt::format("{}s", s);
  if (s < 3600) return fmt::format("{}m{:02}s", s / 60, s % 60);
  return fmt::format("{}h{:02}m{:02}s", s / 3600, (s / 60) % 60, s % 60);
}

std::string money(std::int64_t raw) {
  char buf[48];
  const std::size_t n = Notional::from_raw(raw).to_decimal(buf);
  return std::string(buf, n);
}

std::string_view name_of(const char* buf, std::size_t cap) {
  return std::string_view(buf, strnlen(buf, cap));
}

const char* paint(bool color, std::string_view state) {
  if (!color) return "";
  if (state == "live" || state == "running") return "\x1b[32m";
  if (state == "down" || state == "stopped" || state == "STALE") return "\x1b[31m";
  return "\x1b[33m";
}
const char* reset(bool color) {
  return color ? "\x1b[0m" : "";
}

std::string_view feed_state_name(std::uint8_t s) noexcept {
  switch (s) {
    case 1:
      return "down";
    case 2:
      return "snapshot";
    case 3:
      return "live";
    case 4:
      return "lost";
    default:
      return "none";
  }
}

std::string_view xdp_mode_name(std::uint8_t m) noexcept {
  switch (m) {
    case 1:
      return "zerocopy";
    case 2:
      return "native_copy";
    case 3:
      return "generic";
    default:
      return "-";
  }
}

void json_latency(std::string& out, std::string_view key, const StatusLatency& l) {
  fmt::format_to(std::back_inserter(out),
                 "\"{}\": {{\"count\": {}, \"p50_ns\": {}, \"p99_ns\": {}, \"p999_ns\": {}, "
                 "\"max_ns\": {}}}",
                 key,
                 l.count,
                 l.p50_ns,
                 l.p99_ns,
                 l.p999_ns,
                 l.max_ns);
}

// Names come from config values; escape what JSON requires.
std::string json_string(std::string_view v) {
  std::string out = "\"";
  for (const char c : v) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      out += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

void append_venues(std::string& out, const StatusSnapshot& s, bool color);
void json_venues(std::string& out, const StatusSnapshot& s);
std::string format_gateway_status(const StatusSnapshot& s, std::int64_t now_ns, bool color);
std::string format_gateway_json(const StatusSnapshot& s);

}  // namespace

struct StatusWriter::Segment : SegmentLayout {};
struct StatusReader::Segment : SegmentLayout {};

std::string_view to_string(StatusRunState s) noexcept {
  switch (s) {
    case StatusRunState::Starting:
      return "starting";
    case StatusRunState::Running:
      return "running";
    case StatusRunState::Stopping:
      return "stopping";
    case StatusRunState::Stopped:
      return "stopped";
  }
  return "?";
}

std::string_view to_string(StatusKind k) noexcept {
  return k == StatusKind::Gateway ? "gateway" : "engine";
}

std::string_view channel_state_name(std::uint8_t s) noexcept {
  switch (s) {
    case 0:
      return "down";
    case 1:
      return "connecting";
    case 2:
      return "live";
    case 3:
      return "stale";
    default:
      return "?";
  }
}

void set_status_name(char* dst, std::size_t capacity, std::string_view s) noexcept {
  if (capacity == 0) return;
  const std::size_t n = std::min(s.size(), capacity - 1);
  std::memcpy(dst, s.data(), n);
  std::memset(dst + n, 0, capacity - n);
}

StatusLatency to_status_latency(const LatencyStats& s) noexcept {
  return StatusLatency{s.count, s.p50, s.p99, s.p999, s.max};
}

std::string status_version_mismatch(std::uint32_t version) {
  return fmt::format(
      "status segment version {} is not readable by this build (version {}); use "
      "fastmm-top from the same build as fastmm-live",
      version,
      kStatusVersion);
}

void set_status_rejects(StatusRejectCount* dst, const RejectCounts& c) {
  const auto reasons = nonzero_rejects(c);
  for (std::size_t i = 0; i < kStatusMaxRejectReasons; ++i) {
    dst[i] = StatusRejectCount{};
    if (i < reasons.size()) {
      dst[i].reason = static_cast<std::uint8_t>(reasons[i].first);
      dst[i].count = reasons[i].second;
    }
  }
}

std::string format_status_rejects(const StatusRejectCount* entries, std::uint64_t total) {
  std::string out;
  if (total == 0) return out;
  std::uint64_t shown = 0;
  for (std::size_t i = 0; i < kStatusMaxRejectReasons; ++i) {
    const StatusRejectCount& e = entries[i];
    if (e.count == 0) continue;
    fmt::format_to(std::back_inserter(out),
                   "{}{} {}",
                   out.empty() ? "" : ", ",
                   to_string(static_cast<RejectReason>(e.reason)),
                   e.count);
    shown += e.count;
  }
  if (total > shown)
    fmt::format_to(std::back_inserter(out), "{}other {}", out.empty() ? "" : ", ", total - shown);
  return out;
}

std::string default_status_path(std::string_view engine_name) {
  return fmt::format("/dev/shm/fastmm-{}.status", engine_name);
}

std::string default_gateway_status_path(std::string_view engine_name) {
  return fmt::format("/dev/shm/fastmm-{}.gw.status", engine_name);
}

StatusWriter::~StatusWriter() {
  close();
}

bool StatusWriter::open(const std::string& path, std::string* error) {
  close();
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    if (error != nullptr) *error = errno_text("open");
    return false;
  }
  if (::ftruncate(fd, static_cast<off_t>(kSegmentBytes)) != 0) {
    if (error != nullptr) *error = errno_text("ftruncate");
    ::close(fd);
    return false;
  }
  void* p = ::mmap(nullptr, kSegmentBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) {
    if (error != nullptr) *error = errno_text("mmap");
    return false;
  }
  seg_ = new (p) Segment();  // zeroed file: a valid, empty layout
  return true;
}

void StatusWriter::publish(const StatusSnapshot& s) noexcept {
  if (seg_ == nullptr) return;
  const std::uint64_t seq = seg_->seq.load(std::memory_order_relaxed);
  seg_->seq.store(seq + 1, std::memory_order_release);  // odd: write in progress
  std::memcpy(&seg_->data, &s, sizeof(StatusSnapshot));
  seg_->data.magic = kStatusMagic;
  seg_->data.version = kStatusVersion;
  seg_->seq.store(seq + 2, std::memory_order_release);
}

void StatusWriter::close() noexcept {
  if (seg_ == nullptr) return;
  ::munmap(seg_, kSegmentBytes);
  seg_ = nullptr;
}

StatusReader::~StatusReader() {
  close();
}

bool StatusReader::open(const std::string& path, std::string* error) {
  close();
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (error != nullptr) *error = errno_text("open");
    return false;
  }
  const off_t size = ::lseek(fd, 0, SEEK_END);
  if (size < static_cast<off_t>(kSegmentBytes)) {
    // Possibly an older, smaller layout: say so rather than "too small".
    std::uint8_t header[kHeaderBytes];
    std::uint64_t magic = 0;
    std::uint32_t version = 0;
    if (size >= static_cast<off_t>(kHeaderBytes) &&
        ::pread(fd, header, kHeaderBytes, 0) == static_cast<ssize_t>(kHeaderBytes)) {
      std::memcpy(&magic, header + sizeof(std::uint64_t), sizeof magic);
      std::memcpy(&version, header + kHeaderBytes - sizeof version, sizeof version);
    }
    const bool other_version = magic == kStatusMagic && version != kStatusVersion;
    if (other_version) refused_version_ = version;
    if (error != nullptr) {
      *error = other_version ? status_version_mismatch(version)
                             : std::string("file too small for a status segment");
    }
    ::close(fd);
    return false;
  }
  void* p = ::mmap(nullptr, kSegmentBytes, PROT_READ, MAP_SHARED, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) {
    if (error != nullptr) *error = errno_text("mmap");
    return false;
  }
  seg_ = static_cast<const Segment*>(p);
  return true;
}

bool StatusReader::read(StatusSnapshot& out) const noexcept {
  if (seg_ == nullptr) return false;
  for (int attempt = 0; attempt < 1000; ++attempt) {
    const std::uint64_t s1 = seg_->seq.load(std::memory_order_acquire);
    if ((s1 & 1U) != 0) continue;
    std::memcpy(&out, &seg_->data, sizeof(StatusSnapshot));
    if (seg_->seq.load(std::memory_order_acquire) != s1) continue;
    return out.magic == kStatusMagic && out.version == kStatusVersion && s1 != 0;
  }
  return false;
}

std::uint32_t StatusReader::segment_version() const noexcept {
  if (seg_ == nullptr) return refused_version_;
  std::uint64_t magic = 0;
  std::uint32_t version = 0;
  const auto* base = reinterpret_cast<const std::uint8_t*>(&seg_->data);
  std::memcpy(&magic, base + offsetof(StatusSnapshot, magic), sizeof magic);
  std::memcpy(&version, base + offsetof(StatusSnapshot, version), sizeof version);
  return magic == kStatusMagic ? version : 0;
}

void StatusReader::close() noexcept {
  refused_version_ = 0;
  if (seg_ == nullptr) return;
  ::munmap(const_cast<void*>(static_cast<const void*>(seg_)), kSegmentBytes);
  seg_ = nullptr;
}

std::string format_status(const StatusSnapshot& s, std::int64_t now_ns, bool color) {
  if (s.kind == StatusKind::Gateway) return format_gateway_status(s, now_ns, color);
  std::string out;
  std::string_view state = to_string(s.state);
  const std::int64_t age_ns = now_ns - s.updated_ns;
  const bool stale = s.state == StatusRunState::Running && age_ns > 3'000'000'000;
  if (stale) state = "STALE";
  fmt::format_to(std::back_inserter(out),
                 "fastmm-top  engine={} strategy={} session={} pid={}{}\n",
                 name_of(s.engine_name, sizeof s.engine_name),
                 name_of(s.strategy, sizeof s.strategy),
                 s.session_id,
                 s.pid,
                 s.dry_run != 0 ? "  [dry-run]" : "");
  // The kill switch next to the state: why the global switch tripped, or that some venue is killed.
  std::string kill;
  if ((s.kill_flags & 1U) != 0) {
    kill = fmt::format("  {}KILLED ({}){}",
                       color ? "\x1b[31m" : "",
                       to_string(static_cast<KillReason>(s.kill_reason)),
                       reset(color));
  } else if (s.kill_flags != 0) {
    kill = fmt::format("  {}VENUE KILLED{}", color ? "\x1b[33m" : "", reset(color));
  }
  if (s.kill_latched != 0) {
    kill += fmt::format("  {}LATCHED{}", color ? "\x1b[31m" : "", reset(color));
  }
  if (s.flatten_state == static_cast<std::uint8_t>(FlattenState::Working)) {
    kill += fmt::format("  {}FLATTENING ({} left){}",
                        color ? "\x1b[33m" : "",
                        s.flatten_instruments_left,
                        reset(color));
  } else if (s.flatten_state == static_cast<std::uint8_t>(FlattenState::TimedOut)) {
    kill += fmt::format("  {}FLATTEN TIMED OUT ({} left){}",
                        color ? "\x1b[31m" : "",
                        s.flatten_instruments_left,
                        reset(color));
  }
  fmt::format_to(std::back_inserter(out),
                 "state      {}{}{}{}  uptime={}  updated {:.1f}s ago\n\n",
                 paint(color, state),
                 state,
                 reset(color),
                 kill,
                 fmt_duration_s(s.updated_ns - s.started_ns),
                 static_cast<double>(std::max<std::int64_t>(age_ns, 0)) / 1e9);
  fmt::format_to(std::back_inserter(out),
                 "engine     events={} book_updates={} orders={} cancels={} replaces={} fills={} "
                 "kills={} venue_kills={} kill_flags={:#x}\n",
                 s.events,
                 s.book_updates,
                 s.orders_sent,
                 s.cancels_sent,
                 s.replaces_sent,
                 s.fills,
                 s.kills,
                 s.venue_kills,
                 s.kill_flags);
  if (s.flatten_state != static_cast<std::uint8_t>(FlattenState::Off)) {
    fmt::format_to(std::back_inserter(out),
                   "flatten    state={} instruments_left={} orders={}\n",
                   to_string(static_cast<FlattenState>(s.flatten_state)),
                   s.flatten_instruments_left,
                   s.flatten_orders);
  }
  const auto rejects = [](std::uint64_t total, const StatusRejectCount* entries) {
    return total == 0 ? std::string("0")
                      : fmt::format("{} ({})", total, format_status_rejects(entries, total));
  };
  fmt::format_to(std::back_inserter(out),
                 "rejects    risk_rejects={} venue_rejects={}\n",
                 rejects(s.risk_rejects, s.risk_reject_reasons),
                 rejects(s.venue_rejects, s.venue_reject_reasons));
  fmt::format_to(std::back_inserter(out),
                 "pnl        realized={} unrealized={} fees={} carried={} budget_used={}\n\n",
                 money(s.realized_pnl_raw),
                 money(s.unrealized_pnl_raw),
                 money(s.fees_raw),
                 money(s.pnl_carry_raw),
                 money(s.pnl_carry_raw + s.realized_pnl_raw + s.unrealized_pnl_raw - s.fees_raw));
  fmt::format_to(std::back_inserter(out),
                 "{:<12} {:>10} {:>10} {:>10} {:>10} {:>10}\n",
                 "latency",
                 "count",
                 "p50",
                 "p99",
                 "p99.9",
                 "max");
  for (std::size_t i = 0; i < static_cast<std::size_t>(LatencyInterval::Count); ++i) {
    const StatusLatency& l = s.latency[i];
    fmt::format_to(std::back_inserter(out),
                   "{:<12} {:>10} {:>10} {:>10} {:>10} {:>10}\n",
                   to_string(static_cast<LatencyInterval>(i)),
                   l.count,
                   fmt_ns(l.p50_ns),
                   fmt_ns(l.p99_ns),
                   fmt_ns(l.p999_ns),
                   fmt_ns(l.max_ns));
  }
  append_venues(out, s, color);
  return out;
}

namespace {

// The venue table and the multicast feeds, of an engine's snapshot or a gateway's.
void append_venues(std::string& out, const StatusSnapshot& s, bool color) {
  fmt::format_to(std::back_inserter(out),
                 "\n{:<14} {:<10} {:<10} {:<10} {:>7} {:>9} {:>7} {:>8} {:>8} {:>9} {:>6} {:>7} "
                 "{:>9} {:>10} {}\n",
                 "venue",
                 "md",
                 "user",
                 "order",
                 "books",
                 "md_msgs",
                 "resyncs",
                 "orders",
                 "cancels",
                 "events",
                 "reconn",
                 "rest_er",
                 "clock_ms",
                 "wire_t2t50",
                 "kill");
  const std::size_t n = std::min<std::size_t>(s.venue_count, kStatusMaxVenues);
  for (std::size_t i = 0; i < n; ++i) {
    const StatusVenue& v = s.venues[i];
    const auto chan = [&](std::uint8_t c) {
      const std::string_view name = channel_state_name(c);
      return fmt::format("{}{:<10}{}", paint(color, name), name, reset(color));
    };
    // A killed venue shows the reason it was tripped for; a live one "-".
    const std::string venue_kill =
        v.killed != 0 ? fmt::format("{}{}{}",
                                    color ? "\x1b[31m" : "",
                                    to_string(static_cast<KillReason>(v.kill_reason)),
                                    reset(color))
                      : std::string("-");
    fmt::format_to(
        std::back_inserter(out),
        "{:<14} {} {} {} {:>7} {:>9} {:>7} {:>8} {:>8} {:>9} {:>6} {:>7} {:>9} {:>10} {}\n",
        name_of(v.name, sizeof v.name),
        chan(v.md),
        chan(v.user),
        chan(v.order),
        fmt::format("{}/{}", v.books_synced, v.books_total),
        v.md_messages,
        v.resyncs,
        v.orders_sent,
        v.cancels_sent,
        v.order_events,
        v.reconnects,
        v.rest_errors,
        v.clock_offset_ms,
        fmt_ns(v.wire_tick_to_trade.p50_ns),
        venue_kill);
  }
  // Multicast feeds: one line per venue that has one.
  bool feed_header = false;
  for (std::size_t i = 0; i < n; ++i) {
    const StatusVenue& v = s.venues[i];
    const StatusFeed& f = v.feed;
    if (f.state == 0) continue;
    if (!feed_header) {
      feed_header = true;
      fmt::format_to(std::back_inserter(out),
                     "\n{:<14} {:<9} {:<18} {:>10} {:>10} {:>10} {:>9} {:>6} {:>8} {:>8} {:>6} "
                     "{:>7} {:>10} {:>10}\n",
                     "feed",
                     "state",
                     "rx",
                     "packets",
                     "pkts_a",
                     "pkts_b",
                     "skew_max",
                     "gaps",
                     "recov",
                     "lost",
                     "snaps",
                     "reorder",
                     "k2t0_p50",
                     "k2t0_p99");
    }
    const std::string_view fstate = feed_state_name(f.state);
    const std::string rx = f.backend == 1   ? fmt::format("af_xdp/{}", xdp_mode_name(f.xdp_mode))
                           : f.backend == 2 ? std::string("dpdk")
                                            : std::string("kernel");
    const std::int64_t skew = std::max(f.line_skew_max_ns[0], f.line_skew_max_ns[1]);
    fmt::format_to(std::back_inserter(out),
                   "{:<14} {}{:<9}{} {:<18} {:>10} {:>10} {:>10} {:>9} {:>6} {:>8} {:>8} {:>6} "
                   "{:>7} {:>10} {:>10}\n",
                   name_of(v.name, sizeof v.name),
                   paint(color, fstate),
                   fstate,
                   reset(color),
                   rx,
                   f.packets,
                   f.line_packets[0],
                   f.line_packets[1],
                   fmt_ns(static_cast<std::uint64_t>(std::max<std::int64_t>(skew, 0))),
                   f.gaps,
                   f.recovered,
                   f.unrecovered,
                   f.snapshot_recoveries,
                   f.reorder_high_water,
                   fmt_ns(f.kernel_to_t0.p50_ns),
                   fmt_ns(f.kernel_to_t0.p99_ns));
    if (f.backend == 1) {
      fmt::format_to(std::back_inserter(out),
                     "{:<14} xdp rx_dropped={} rx_invalid_descs={} rx_ring_full={} "
                     "fill_ring_empty={} fallback={}\n",
                     "",
                     f.xdp_rx_dropped,
                     f.xdp_rx_invalid_descs,
                     f.xdp_rx_ring_full,
                     f.xdp_fill_ring_empty,
                     f.xdp_fallback);
    }
  }
}

std::string qty_text(std::int64_t raw) {
  char buf[48];
  const std::size_t n = Qty::from_raw(raw).to_decimal(buf);
  return std::string(buf, n);
}

std::string limit_text(std::int64_t raw) {
  return raw > 0 ? money(raw) : std::string("off");
}

// "0", or "5 (GatewayRateLimit 3, GatewayOpenNotional 2)".
std::string refusals_text(const std::uint64_t (&r)[kStatusGatewayRefusals]) {
  std::uint64_t total = 0;
  std::string reasons;
  for (std::size_t i = 0; i < kStatusGatewayRefusals; ++i) {
    if (r[i] == 0) continue;
    total += r[i];
    fmt::format_to(std::back_inserter(reasons),
                   "{}{} {}",
                   reasons.empty() ? "" : ", ",
                   to_string(kStatusGatewayRefusalReasons[i]),
                   r[i]);
  }
  return total == 0 ? std::string("0") : fmt::format("{} ({})", total, reasons);
}

std::string_view venue_name(const StatusSnapshot& s, std::uint8_t v) {
  return v < kStatusMaxVenues ? name_of(s.venues[v].name, sizeof s.venues[v].name)
                              : std::string_view("?");
}

// "sim:BTCUSDT,sim:ETHUSDT": the instruments an attachment owns.
std::string owned_instruments(const StatusSnapshot& s, std::uint16_t epoch) {
  std::string out;
  const std::size_t n = std::min<std::size_t>(s.gateway.position_count, kStatusMaxPositions);
  for (std::size_t i = 0; i < n; ++i) {
    const StatusPosition& p = s.gateway.positions[i];
    if (p.owner_epoch != epoch) continue;
    fmt::format_to(std::back_inserter(out),
                   "{}{}:{}",
                   out.empty() ? "" : ",",
                   venue_name(s, p.venue),
                   name_of(p.symbol, sizeof p.symbol));
  }
  return out;
}

std::string format_gateway_status(const StatusSnapshot& s, std::int64_t now_ns, bool color) {
  const StatusGateway& g = s.gateway;
  std::string out;
  auto it = std::back_inserter(out);
  std::string_view state = to_string(s.state);
  const std::int64_t age_ns = now_ns - s.updated_ns;
  if (s.state == StatusRunState::Running && age_ns > 3'000'000'000) state = "STALE";
  fmt::format_to(it,
                 "fastmm-top  gateway={} pid={}{}\n",
                 name_of(s.engine_name, sizeof s.engine_name),
                 s.pid,
                 s.dry_run != 0 ? "  [dry-run]" : "");
  std::string kill;
  if (g.kill_active != 0) {
    kill = fmt::format("  {}ACCOUNT KILLED ({}){}",
                       color ? "\x1b[31m" : "",
                       to_string(static_cast<KillReason>(s.kill_reason)),
                       reset(color));
  }
  if (s.kill_latched != 0)
    kill += fmt::format("  {}LATCHED{}", color ? "\x1b[31m" : "", reset(color));
  fmt::format_to(it,
                 "state      {}{}{}{}  uptime={}  updated {:.1f}s ago\n\n",
                 paint(color, state),
                 state,
                 reset(color),
                 kill,
                 fmt_duration_s(s.updated_ns - s.started_ns),
                 static_cast<double>(std::max<std::int64_t>(age_ns, 0)) / 1e9);
  fmt::format_to(it,
                 "account    net_pnl={} realized={} unrealized={} fees={} carried={} "
                 "gross_exposure={} net_exposure={}\n",
                 money(g.net_pnl_raw),
                 money(s.realized_pnl_raw),
                 money(s.unrealized_pnl_raw),
                 money(s.fees_raw),
                 money(s.pnl_carry_raw),
                 money(g.gross_raw),
                 money(g.net_raw));
  fmt::format_to(it,
                 "limits     max_loss={} max_gross_notional={} max_net_notional={} "
                 "max_open_notional={}\n\n",
                 limit_text(g.max_loss_raw),
                 limit_text(g.max_gross_raw),
                 limit_text(g.max_net_raw),
                 limit_text(g.max_open_notional_raw));

  const std::size_t na = std::min<std::size_t>(g.attachment_count, kStatusMaxAttachments);
  fmt::format_to(it,
                 "{:<10} {:>5} {:<20} {:>8} {:>9} {:>10} {:<24} {}\n",
                 "attachment",
                 "epoch",
                 "engine",
                 "pid",
                 "up",
                 "md_dropped",
                 "refused",
                 "instruments");
  if (na == 0) out += "(none attached)\n";
  for (std::size_t i = 0; i < na; ++i) {
    const StatusAttachment& a = g.attachments[i];
    fmt::format_to(it,
                   "{:<10} {:>5} {:<20} {:>8} {:>9} {:>10} {:<24} {}\n",
                   a.id,
                   a.epoch,
                   name_of(a.engine, sizeof a.engine),
                   a.pid,
                   fmt_duration_s(s.updated_ns - a.attached_ns),
                   a.md_dropped,
                   refusals_text(a.refused),
                   owned_instruments(s, a.epoch));
  }

  // The instruments with a position or an owner.
  fmt::format_to(it, "\n{:<14} {:<20} {:>20} {:>6}\n", "position", "instrument", "qty", "owner");
  const std::size_t np = std::min<std::size_t>(g.position_count, kStatusMaxPositions);
  for (std::size_t i = 0; i < np; ++i) {
    const StatusPosition& p = g.positions[i];
    if (p.qty_raw == 0 && p.owner_epoch == 0) continue;
    fmt::format_to(it,
                   "{:<14} {:<20} {:>20} {:>6}\n",
                   venue_name(s, p.venue),
                   name_of(p.symbol, sizeof p.symbol),
                   qty_text(p.qty_raw),
                   p.owner_epoch != 0 ? std::to_string(p.owner_epoch) : std::string("-"));
  }

  append_venues(out, s, color);

  fmt::format_to(it,
                 "\n{:<14} {:>10} {:>10} {:>9} {:>10} {:>9} {:>7} {:>8} {:>10} {}\n",
                 "routing",
                 "md_discard",
                 "ord_discard",
                 "unrouted",
                 "gw_cancels",
                 "untracked",
                 "stale",
                 "skipped",
                 "books_lost",
                 "refused");
  const std::size_t nv = std::min<std::size_t>(s.venue_count, kStatusMaxVenues);
  for (std::size_t i = 0; i < nv; ++i) {
    const StatusGatewayVenue& v = g.venues[i];
    fmt::format_to(it,
                   "{:<14} {:>10} {:>10} {:>9} {:>10} {:>9} {:>7} {:>8} {:>10} {}\n",
                   name_of(s.venues[i].name, sizeof s.venues[i].name),
                   v.md_discarded,
                   v.order_discarded,
                   v.unrouted,
                   v.gateway_cancels,
                   v.untracked,
                   v.stale_replays,
                   v.account_skipped,
                   v.account_md_lost,
                   refusals_text(v.refused));
  }
  return out;
}

}  // namespace

std::string format_status_json(const StatusSnapshot& s) {
  if (s.kind == StatusKind::Gateway) return format_gateway_json(s);
  std::string out;
  auto it = std::back_inserter(out);
  fmt::format_to(it,
                 "{{\"kind\": \"engine\", \"version\": {}, \"pid\": {}, \"session_id\": {}, "
                 "\"state\": \"{}\", "
                 "\"engine\": {}, \"strategy\": {}, \"events\": {}, \"book_updates\": {}, "
                 "\"orders_sent\": {}, \"cancels_sent\": {}, \"replaces_sent\": {}, \"fills\": {}, "
                 "\"risk_rejects\": {}, \"venue_rejects\": {}, \"kill_flags\": {}, "
                 "\"kill_reason\": \"{}\", \"kill_latched\": {}, \"pnl_carry_raw\": {}, "
                 "\"flatten_state\": \"{}\", \"flatten_instruments_left\": {}, "
                 "\"flatten_orders\": {}, \"latency\": {{",
                 s.version,
                 s.pid,
                 s.session_id,
                 to_string(s.state),
                 json_string(name_of(s.engine_name, sizeof s.engine_name)),
                 json_string(name_of(s.strategy, sizeof s.strategy)),
                 s.events,
                 s.book_updates,
                 s.orders_sent,
                 s.cancels_sent,
                 s.replaces_sent,
                 s.fills,
                 s.risk_rejects,
                 s.venue_rejects,
                 s.kill_flags,
                 to_string(static_cast<KillReason>(s.kill_reason)),
                 s.kill_latched != 0,
                 s.pnl_carry_raw,
                 to_string(static_cast<FlattenState>(s.flatten_state)),
                 s.flatten_instruments_left,
                 s.flatten_orders);
  for (std::size_t i = 0; i < static_cast<std::size_t>(LatencyInterval::Count); ++i) {
    if (i != 0) out += ", ";
    json_latency(out, to_string(static_cast<LatencyInterval>(i)), s.latency[i]);
  }
  out += "}, ";
  json_venues(out, s);
  out += "}\n";
  return out;
}

namespace {

void json_venues(std::string& out, const StatusSnapshot& s) {
  auto it = std::back_inserter(out);
  out += "\"venues\": [";
  const std::size_t n = std::min<std::size_t>(s.venue_count, kStatusMaxVenues);
  for (std::size_t i = 0; i < n; ++i) {
    const StatusVenue& v = s.venues[i];
    const StatusFeed& f = v.feed;
    if (i != 0) out += ", ";
    fmt::format_to(it,
                   "{{\"name\": {}, \"md\": \"{}\", \"order\": \"{}\", \"killed\": {}, "
                   "\"kill_reason\": \"{}\", \"books_synced\": {}, \"books_total\": {}, "
                   "\"md_messages\": {}, \"resyncs\": {}, \"orders_sent\": {}, "
                   "\"cancels_sent\": {}, \"order_events\": {}, ",
                   json_string(name_of(v.name, sizeof v.name)),
                   channel_state_name(v.md),
                   channel_state_name(v.order),
                   v.killed != 0 ? "true" : "false",
                   to_string(static_cast<KillReason>(v.kill_reason)),
                   v.books_synced,
                   v.books_total,
                   v.md_messages,
                   v.resyncs,
                   v.orders_sent,
                   v.cancels_sent,
                   v.order_events);
    json_latency(out, "wire_tick_to_trade", v.wire_tick_to_trade);
    fmt::format_to(it,
                   ", \"feed\": {{\"state\": \"{}\", \"backend\": \"{}\", \"xdp_mode\": \"{}\", "
                   "\"packets\": {}, \"bytes\": {}, \"line_packets\": [{}, {}], "
                   "\"line_duplicates\": [{}, {}], \"line_skew_mean_ns\": [{}, {}], "
                   "\"line_skew_max_ns\": [{}, {}], \"gaps\": {}, \"recovered\": {}, "
                   "\"unrecovered\": {}, \"snapshot_recoveries\": {}, \"recovery_overflows\": {}, "
                   "\"reorder_high_water\": {}, \"requests\": {}, \"malformed\": {}, "
                   "\"book_errors\": {}, ",
                   feed_state_name(f.state),
                   f.backend == 1   ? "af_xdp"
                   : f.backend == 2 ? "dpdk"
                                    : "kernel",
                   xdp_mode_name(f.xdp_mode),
                   f.packets,
                   f.bytes,
                   f.line_packets[0],
                   f.line_packets[1],
                   f.line_duplicates[0],
                   f.line_duplicates[1],
                   f.line_skew_mean_ns[0],
                   f.line_skew_mean_ns[1],
                   f.line_skew_max_ns[0],
                   f.line_skew_max_ns[1],
                   f.gaps,
                   f.recovered,
                   f.unrecovered,
                   f.snapshot_recoveries,
                   f.recovery_overflows,
                   f.reorder_high_water,
                   f.requests,
                   f.malformed,
                   f.book_errors);
    json_latency(out, "kernel_to_t0", f.kernel_to_t0);
    fmt::format_to(it,
                   ", \"xdp_rx_dropped\": {}, \"xdp_rx_invalid_descs\": {}, \"xdp_rx_ring_full\": "
                   "{}, \"xdp_fill_ring_empty\": {}, \"xdp_fallback\": {}}}}}",
                   f.xdp_rx_dropped,
                   f.xdp_rx_invalid_descs,
                   f.xdp_rx_ring_full,
                   f.xdp_fill_ring_empty,
                   f.xdp_fallback);
  }
  out += "]";
}

void json_refusals(std::string& out, const std::uint64_t (&r)[kStatusGatewayRefusals]) {
  out += "{";
  for (std::size_t i = 0; i < kStatusGatewayRefusals; ++i) {
    fmt::format_to(std::back_inserter(out),
                   "{}\"{}\": {}",
                   i == 0 ? "" : ", ",
                   to_string(kStatusGatewayRefusalReasons[i]),
                   r[i]);
  }
  out += "}";
}

std::string format_gateway_json(const StatusSnapshot& s) {
  const StatusGateway& g = s.gateway;
  std::string out;
  auto it = std::back_inserter(out);
  fmt::format_to(it,
                 "{{\"kind\": \"gateway\", \"version\": {}, \"pid\": {}, \"state\": \"{}\", "
                 "\"gateway\": {}, \"dry_run\": {}, \"started_ns\": {}, \"updated_ns\": {}, "
                 "\"kill_active\": {}, \"kill_reason\": \"{}\", \"kill_latched\": {}, "
                 "\"account\": {{\"net_pnl\": {}, \"realized\": {}, \"unrealized\": {}, "
                 "\"fees\": {}, \"carried\": {}, \"gross_exposure\": {}, \"net_exposure\": {}, "
                 "\"trip_net_pnl\": {}, \"max_loss\": {}, \"max_gross_notional\": {}, "
                 "\"max_net_notional\": {}, \"max_open_notional\": {}}}, \"attachments\": [",
                 s.version,
                 s.pid,
                 to_string(s.state),
                 json_string(name_of(s.engine_name, sizeof s.engine_name)),
                 s.dry_run != 0,
                 s.started_ns,
                 s.updated_ns,
                 g.kill_active != 0,
                 to_string(static_cast<KillReason>(s.kill_reason)),
                 s.kill_latched != 0,
                 money(g.net_pnl_raw),
                 money(s.realized_pnl_raw),
                 money(s.unrealized_pnl_raw),
                 money(s.fees_raw),
                 money(s.pnl_carry_raw),
                 money(g.gross_raw),
                 money(g.net_raw),
                 money(g.trip_net_raw),
                 money(g.max_loss_raw),
                 money(g.max_gross_raw),
                 money(g.max_net_raw),
                 money(g.max_open_notional_raw));
  const std::size_t na = std::min<std::size_t>(g.attachment_count, kStatusMaxAttachments);
  const std::size_t np = std::min<std::size_t>(g.position_count, kStatusMaxPositions);
  for (std::size_t i = 0; i < na; ++i) {
    const StatusAttachment& a = g.attachments[i];
    fmt::format_to(it,
                   "{}{{\"id\": {}, \"epoch\": {}, \"engine\": {}, \"pid\": {}, \"blocks\": {}, "
                   "\"attached_ns\": {}, \"md_dropped\": {}, \"refused\": ",
                   i == 0 ? "" : ", ",
                   a.id,
                   a.epoch,
                   json_string(name_of(a.engine, sizeof a.engine)),
                   a.pid,
                   a.blocks != 0,
                   a.attached_ns,
                   a.md_dropped);
    json_refusals(out, a.refused);
    out += ", \"instruments\": [";
    bool first = true;
    for (std::size_t k = 0; k < np; ++k) {
      const StatusPosition& p = g.positions[k];
      if (p.owner_epoch != a.epoch) continue;
      fmt::format_to(it,
                     "{}{{\"venue\": {}, \"symbol\": {}}}",
                     first ? "" : ", ",
                     json_string(venue_name(s, p.venue)),
                     json_string(name_of(p.symbol, sizeof p.symbol)));
      first = false;
    }
    out += "]}";
  }
  out += "], \"positions\": [";
  for (std::size_t i = 0; i < np; ++i) {
    const StatusPosition& p = g.positions[i];
    fmt::format_to(it,
                   "{}{{\"venue\": {}, \"symbol\": {}, \"qty\": {}, \"owner_epoch\": {}}}",
                   i == 0 ? "" : ", ",
                   json_string(venue_name(s, p.venue)),
                   json_string(name_of(p.symbol, sizeof p.symbol)),
                   qty_text(p.qty_raw),
                   p.owner_epoch);
  }
  out += "], \"routing\": [";
  const std::size_t nv = std::min<std::size_t>(s.venue_count, kStatusMaxVenues);
  for (std::size_t i = 0; i < nv; ++i) {
    const StatusGatewayVenue& v = g.venues[i];
    fmt::format_to(it,
                   "{}{{\"venue\": {}, \"md_discarded\": {}, \"order_discarded\": {}, "
                   "\"unrouted\": {}, \"gateway_cancels\": {}, \"untracked\": {}, "
                   "\"stale_replays\": {}, \"account_skipped\": {}, \"account_md_lost\": {}, "
                   "\"realized\": {}, \"unrealized\": {}, \"fees\": {}, \"gross_exposure\": {}, "
                   "\"net_exposure\": {}, \"refused\": ",
                   i == 0 ? "" : ", ",
                   json_string(venue_name(s, static_cast<std::uint8_t>(i))),
                   v.md_discarded,
                   v.order_discarded,
                   v.unrouted,
                   v.gateway_cancels,
                   v.untracked,
                   v.stale_replays,
                   v.account_skipped,
                   v.account_md_lost,
                   money(v.realized_raw),
                   money(v.unrealized_raw),
                   money(v.fees_raw),
                   money(v.gross_raw),
                   money(v.net_raw));
    json_refusals(out, v.refused);
    out += "}";
  }
  out += "], ";
  json_venues(out, s);
  out += "}\n";
  return out;
}

}  // namespace

}  // namespace fastmm
