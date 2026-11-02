#include "fastmm/core/shadow_status.hpp"

#include <fmt/format.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace fastmm {

namespace {

static_assert(std::is_trivially_copyable_v<ShadowStatus>);

struct Segment {
  std::atomic<std::uint64_t> seq;
  ShadowStatus status;
};

// NaN and infinities have no spelling in JSON or Prometheus' gauges we want to chart.
std::string json_num(double v) {
  return std::isfinite(v) ? fmt::format("{:.6g}", v) : std::string("null");
}

std::string money(double v) {
  return std::isfinite(v) ? fmt::format("{:+.2f}", v) : std::string("n/a");
}

std::string ratio(double a, double b) {
  return b != 0.0 ? fmt::format("{:.2f}", a / b) : std::string("n/a");
}

std::string json_text(std::string_view v) {
  std::string out = "\"";
  for (const char c : v) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

std::string text_of(const char* s, std::size_t cap) {
  return {s, strnlen(s, cap)};
}

std::string side_json(const ShadowStatusSide& s) {
  return fmt::format("{{\"net\": {}, \"notional\": {}, \"fills\": {}, \"orders\": {}}}",
                     json_num(s.net),
                     json_num(s.notional),
                     s.fills,
                     s.orders);
}

}  // namespace

const char* to_string(ShadowState s) noexcept {
  switch (s) {
    case ShadowState::Starting:
      return "starting";
    case ShadowState::CatchingUp:
      return "catching_up";
    case ShadowState::Following:
      return "following";
    case ShadowState::Waiting:
      return "waiting";
    case ShadowState::Stopped:
      return "stopped";
  }
  return "?";
}

const char* to_string(ShadowVerdict v) noexcept {
  switch (v) {
    case ShadowVerdict::Unclear:
      return "unclear";
    case ShadowVerdict::Better:
      return "better";
    case ShadowVerdict::Worse:
      return "worse";
    case ShadowVerdict::Stopped:
      return "stopped";
  }
  return "?";
}

std::string default_shadow_status_path(const std::string& engine_name) {
  return "/dev/shm/fastmm-" + engine_name + ".shadow";
}

ShadowStatusWriter::~ShadowStatusWriter() {
  if (map_ != nullptr) ::munmap(map_, len_);
}

bool ShadowStatusWriter::open(const std::string& path, std::string* error) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    if (error != nullptr) *error = std::strerror(errno);
    return false;
  }
  len_ = sizeof(Segment);
  if (::ftruncate(fd, static_cast<off_t>(len_)) != 0) {
    if (error != nullptr) *error = std::strerror(errno);
    ::close(fd);
    return false;
  }
  void* p = ::mmap(nullptr, len_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) {
    if (error != nullptr) *error = std::strerror(errno);
    return false;
  }
  map_ = p;
  return true;
}

void ShadowStatusWriter::write(const ShadowStatus& s) noexcept {
  if (map_ == nullptr) return;
  auto* seg = static_cast<Segment*>(map_);
  const std::uint64_t q = seg->seq.load(std::memory_order_relaxed);
  seg->seq.store(q + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  std::memcpy(static_cast<void*>(&seg->status), &s, sizeof s);
  seg->seq.store(q + 2, std::memory_order_release);
}

ShadowStatusReader::~ShadowStatusReader() {
  if (map_ != nullptr) ::munmap(const_cast<void*>(map_), len_);
}

bool ShadowStatusReader::open(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st {};
  if (::fstat(fd, &st) != 0 || static_cast<std::size_t>(st.st_size) != sizeof(Segment)) {
    ::close(fd);
    return false;
  }
  void* p = ::mmap(nullptr, sizeof(Segment), PROT_READ, MAP_SHARED, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) return false;
  map_ = p;
  len_ = sizeof(Segment);
  return true;
}

bool ShadowStatusReader::read(ShadowStatus& out) const noexcept {
  if (map_ == nullptr) return false;
  const auto* seg = static_cast<const Segment*>(map_);
  for (int attempt = 0; attempt < 64; ++attempt) {
    const std::uint64_t a = seg->seq.load(std::memory_order_acquire);
    if (a == 0) return false;  // nothing written yet
    if ((a & 1U) != 0) continue;
    std::memcpy(static_cast<void*>(&out), &seg->status, sizeof out);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seg->seq.load(std::memory_order_relaxed) == a)
      return out.magic == kShadowStatusMagic && out.version == kShadowStatusVersion;
  }
  return false;
}

std::string format_shadow_status(const ShadowStatus& s, std::int64_t now_ns) {
  std::string o = fmt::format(
      "\nshadow  {}  {} points{}  {} windows of {} s  {:.1f} s behind  updated {:.1f} s ago\n",
      to_string(static_cast<ShadowState>(s.state)),
      s.points,
      s.stopped_points != 0 ? fmt::format(" ({} stopped)", s.stopped_points) : std::string(),
      s.windows,
      s.window_ns / 1'000'000'000,
      static_cast<double>(s.lag_ns) / 1e9,
      static_cast<double>(now_ns - s.updated_ns) / 1e9);
  if (s.late != 0 || s.seq_gaps != 0)
    o += fmt::format(
        "  {} late records, {} sequence gaps: may differ from a finished journal's "
        "backtest\n",
        s.late,
        s.seq_gaps);
  o += fmt::format("  {:<14}{:>12}{:>12}{:>8}{:>10}\n", "", "live", "simulated", "ratio", "error");
  const auto row =
      [&](const char* name, const ShadowStatusSide& live, const ShadowStatusSide& sim) {
        o += fmt::format("  {:<14}{:>12}{:>12}{:>8}{:>10}\n",
                         fmt::format("{} net", name),
                         money(live.net),
                         money(sim.net),
                         "",
                         money(sim.net - live.net));
        o += fmt::format("  {:<14}{:>12.0f}{:>12.0f}{:>8}\n",
                         fmt::format("{} notional", name),
                         live.notional,
                         sim.notional,
                         ratio(sim.notional, live.notional));
        o += fmt::format("  {:<14}{:>12}{:>12}{:>8}\n",
                         fmt::format("{} fills", name),
                         live.fills,
                         sim.fills,
                         ratio(static_cast<double>(sim.fills), static_cast<double>(live.fills)));
      };
  row("last", s.last_live, s.last_sim);
  row("all", s.live, s.sim);
  if (std::isfinite(s.net_error_lo)) {
    o += fmt::format(
        "  error on net, 95%: {} to {}\n", money(s.net_error_lo), money(s.net_error_hi));
  }
  if (s.instruments != 0) {
    o += fmt::format("  {:<20}{:>11}{:>11}{:>11}{:>11}\n",
                     "widest gaps",
                     "live net",
                     "sim net",
                     "live pos",
                     "sim pos");
    for (std::uint32_t i = 0; i < s.instruments; ++i) {
      const ShadowStatusInstrument& r = s.instrument[i];
      o += fmt::format("  {:<20}{:>11}{:>11}{:>11.4g}{:>11.4g}\n",
                       text_of(r.symbol, sizeof r.symbol),
                       money(r.live_net),
                       money(r.sim_net),
                       r.live_position,
                       r.sim_position);
    }
  }
  if (s.variants != 0) {
    o += fmt::format("  {:<38}{:>11}{:>23}{:>9}\n", "variants", "vs live", "95% interval", "");
    for (std::uint32_t i = 0; i < s.variants; ++i) {
      const ShadowStatusVariant& v = s.variant[i];
      o += fmt::format("  {:<38}{:>11}{:>23}{:>9}\n",
                       text_of(v.label, sizeof v.label),
                       money(v.diff),
                       std::isfinite(v.lo) ? fmt::format("{} to {}", money(v.lo), money(v.hi))
                                           : std::string("n/a"),
                       to_string(static_cast<ShadowVerdict>(v.verdict)));
    }
  }
  return o;
}

std::string shadow_status_json(const ShadowStatus& s) {
  std::string variants;
  for (std::uint32_t i = 0; i < s.variants; ++i) {
    const ShadowStatusVariant& v = s.variant[i];
    variants += fmt::format(
        "{}{{\"label\": {}, \"diff\": {}, \"lo\": {}, \"hi\": {}, \"notional\": {}, \"fills\": {}, "
        "\"verdict\": \"{}\"}}",
        i == 0 ? "" : ", ",
        json_text(text_of(v.label, sizeof v.label)),
        json_num(v.diff),
        json_num(v.lo),
        json_num(v.hi),
        json_num(v.notional),
        v.fills,
        to_string(static_cast<ShadowVerdict>(v.verdict)));
  }
  return fmt::format(
      "{{\"state\": \"{}\", \"session_id\": {}, \"points\": {}, \"stopped_points\": {}, "
      "\"sessions\": {}, \"windows\": {}, \"lag_s\": {}, \"late\": {}, \"seq_gaps\": {}, "
      "\"live\": {}, \"sim\": {}, \"net_error_lo\": {}, \"net_error_hi\": {}, \"last_live\": {}, "
      "\"last_sim\": {}, \"variants\": [{}]}}",
      to_string(static_cast<ShadowState>(s.state)),
      s.session_id,
      s.points,
      s.stopped_points,
      s.sessions,
      s.windows,
      json_num(static_cast<double>(s.lag_ns) / 1e9),
      s.late,
      s.seq_gaps,
      side_json(s.live),
      side_json(s.sim),
      json_num(s.net_error_lo),
      json_num(s.net_error_hi),
      side_json(s.last_live),
      side_json(s.last_sim),
      variants);
}

std::string shadow_status_prometheus(const ShadowStatus& s, std::int64_t now_ns) {
  std::string o;
  const auto gauge = [&](std::string_view name, std::string_view help, double v) {
    if (!std::isfinite(v)) return;
    o += fmt::format("# HELP {0} {1}\n# TYPE {0} gauge\n{0} {2:.10g}\n", name, help, v);
  };
  gauge("fastmm_shadow_up", "1 while a shadow publishes", 1);
  gauge("fastmm_shadow_status_age_seconds",
        "time since the shadow last published",
        static_cast<double>(now_ns - s.updated_ns) / 1e9);
  gauge("fastmm_shadow_state",
        "0 starting, 1 catching up, 2 following, 3 waiting, 4 stopped",
        s.state);
  gauge("fastmm_shadow_lag_seconds",
        "the wall clock less the venue time the simulation has reached",
        static_cast<double>(s.lag_ns) / 1e9);
  gauge("fastmm_shadow_points",
        "points running, the live point included",
        s.points - s.stopped_points);
  gauge("fastmm_shadow_windows", "windows closed", s.windows);
  gauge("fastmm_shadow_late_records",
        "records later than settle_s: 0 is exact",
        static_cast<double>(s.late));
  gauge("fastmm_shadow_live_net", "live net PnL over the windows closed", s.live.net);
  gauge("fastmm_shadow_sim_net", "simulated net PnL at the live parameters", s.sim.net);
  gauge("fastmm_shadow_net_error", "simulated less live net PnL", s.sim.net - s.live.net);
  gauge("fastmm_shadow_net_error_lo", "95% interval of the error, low", s.net_error_lo);
  gauge("fastmm_shadow_net_error_hi", "95% interval of the error, high", s.net_error_hi);
  gauge("fastmm_shadow_live_notional", "live traded notional", s.live.notional);
  gauge("fastmm_shadow_sim_notional",
        "simulated traded notional at the live parameters",
        s.sim.notional);
  gauge("fastmm_shadow_live_fills", "live fills", static_cast<double>(s.live.fills));
  gauge("fastmm_shadow_sim_fills",
        "simulated fills at the live parameters",
        static_cast<double>(s.sim.fills));
  if (s.variants != 0) {
    o += "# HELP fastmm_shadow_variant_diff net PnL of the variant less the live parameters\n"
         "# TYPE fastmm_shadow_variant_diff gauge\n";
    for (std::uint32_t i = 0; i < s.variants; ++i) {
      const ShadowStatusVariant& v = s.variant[i];
      if (!std::isfinite(v.diff)) continue;
      o += fmt::format("fastmm_shadow_variant_diff{{variant={}}} {:.10g}\n",
                       json_text(text_of(v.label, sizeof v.label)),
                       v.diff);
    }
  }
  return o;
}

}  // namespace fastmm
