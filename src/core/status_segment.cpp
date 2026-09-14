#include "fastmm/core/status_segment.hpp"

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
  return StatusLatency{s.count, s.p50, s.p99, s.max};
}

std::string default_status_path(std::string_view engine_name) {
  return fmt::format("/dev/shm/fastmm-{}.status", engine_name);
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
    if (error != nullptr) *error = "file too small for a status segment";
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

void StatusReader::close() noexcept {
  if (seg_ == nullptr) return;
  ::munmap(const_cast<void*>(static_cast<const void*>(seg_)), kSegmentBytes);
  seg_ = nullptr;
}

std::string format_status(const StatusSnapshot& s, std::int64_t now_ns, bool color) {
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
  fmt::format_to(std::back_inserter(out),
                 "state      {}{}{}  uptime={}  updated {:.1f}s ago\n\n",
                 paint(color, state),
                 state,
                 reset(color),
                 fmt_duration_s(s.updated_ns - s.started_ns),
                 static_cast<double>(std::max<std::int64_t>(age_ns, 0)) / 1e9);
  fmt::format_to(std::back_inserter(out),
                 "engine     events={} book_updates={} orders={} cancels={} replaces={} fills={} "
                 "risk_rejects={} kills={} kill_flags={:#x}\n",
                 s.events,
                 s.book_updates,
                 s.orders_sent,
                 s.cancels_sent,
                 s.replaces_sent,
                 s.fills,
                 s.risk_rejects,
                 s.kills,
                 s.kill_flags);
  fmt::format_to(std::back_inserter(out),
                 "pnl        realized={} unrealized={} fees={}\n\n",
                 money(s.realized_pnl_raw),
                 money(s.unrealized_pnl_raw),
                 money(s.fees_raw));
  fmt::format_to(std::back_inserter(out),
                 "{:<12} {:>10} {:>10} {:>10} {:>10}\n",
                 "latency",
                 "count",
                 "p50",
                 "p99",
                 "max");
  for (std::size_t i = 0; i < static_cast<std::size_t>(LatencyInterval::Count); ++i) {
    const StatusLatency& l = s.latency[i];
    fmt::format_to(std::back_inserter(out),
                   "{:<12} {:>10} {:>10} {:>10} {:>10}\n",
                   to_string(static_cast<LatencyInterval>(i)),
                   l.count,
                   fmt_ns(l.p50_ns),
                   fmt_ns(l.p99_ns),
                   fmt_ns(l.max_ns));
  }
  fmt::format_to(std::back_inserter(out),
                 "\n{:<14} {:<10} {:<10} {:<10} {:>7} {:>9} {:>7} {:>8} {:>8} {:>9} {:>6} {:>7} "
                 "{:>9} {:>10}\n",
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
                 "wire_t2t50");
  const std::size_t n = std::min<std::size_t>(s.venue_count, kStatusMaxVenues);
  for (std::size_t i = 0; i < n; ++i) {
    const StatusVenue& v = s.venues[i];
    const auto chan = [&](std::uint8_t c) {
      const std::string_view name = channel_state_name(c);
      return fmt::format("{}{:<10}{}", paint(color, name), name, reset(color));
    };
    fmt::format_to(std::back_inserter(out),
                   "{:<14} {} {} {} {:>7} {:>9} {:>7} {:>8} {:>8} {:>9} {:>6} {:>7} {:>9} {:>10}\n",
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
                   fmt_ns(v.wire_tick_to_trade.p50_ns));
  }
  return out;
}

}  // namespace fastmm
