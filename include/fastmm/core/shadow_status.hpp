#pragma once
// The status file of fastmm-shadow, for fastmm-top: how far the simulation is from the live session
// right now and how the best variants compare (backtest/shadow.hpp). Plain old data behind a
// sequence counter, like the session's own status file (status_segment.hpp), which it sits beside:
// a reader copies the snapshot and keeps it when the counter was even and unchanged across the
// copy. The shadow writes it from its own process; the session never touches it.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace fastmm {

inline constexpr std::uint64_t kShadowStatusMagic = 0x3144414853464DULL;  // "MFSHAD1"
inline constexpr std::uint32_t kShadowStatusVersion = 1;
inline constexpr std::size_t kShadowStatusVariants = 8;
inline constexpr std::size_t kShadowStatusInstruments = 8;

enum class ShadowState : std::uint8_t {
  Starting = 0,    // reading the start of the session
  CatchingUp = 1,  // behind the end of the journal, reading as fast as it can
  Following = 2,   // at the end of the journal
  Waiting = 3,     // between sessions: the last one ended and no other has begun
  Stopped = 4,
};
[[nodiscard]] const char* to_string(ShadowState s) noexcept;

// A variant's verdict against the live parameters: its simultaneous 95% interval on the difference
// of net PnL lies above, or below, how far the simulation is from live at the live parameters.
enum class ShadowVerdict : std::uint8_t { Unclear = 0, Better = 1, Worse = 2, Stopped = 3 };
[[nodiscard]] const char* to_string(ShadowVerdict v) noexcept;

struct ShadowStatusSide {
  double net = 0.0;  // net PnL, settlement or reporting currency
  double notional = 0.0;
  std::uint64_t fills = 0;
  std::uint64_t orders = 0;
};

struct ShadowStatusVariant {
  char label[56] = {};
  double diff = 0.0;  // its net PnL less the live parameters', in the simulation
  double lo = 0.0;    // simultaneous 95% interval; NaN until there are enough windows
  double hi = 0.0;
  double notional = 0.0;
  std::uint64_t fills = 0;
  std::uint8_t verdict = 0;  // ShadowVerdict
  std::uint8_t pad_[7] = {};
};

struct ShadowStatusInstrument {
  char symbol[32] = {};
  double live_net = 0.0;
  double sim_net = 0.0;
  double live_notional = 0.0;
  double sim_notional = 0.0;
  double live_position = 0.0;
  double sim_position = 0.0;
};

struct ShadowStatus {
  std::uint64_t magic = kShadowStatusMagic;
  std::uint32_t version = kShadowStatusVersion;
  std::uint32_t pid = 0;
  std::uint64_t session_id = 0;
  std::int64_t started_ns = 0;
  std::int64_t updated_ns = 0;
  std::uint8_t state = 0;  // ShadowState
  std::uint8_t pad0_[7] = {};
  char strategy[32] = {};
  std::uint32_t points = 0;  // the live point and the variants
  std::uint32_t stopped_points = 0;
  std::uint32_t sessions = 0;  // sessions followed so far, this one included
  std::uint32_t windows = 0;   // windows closed, over all of them
  std::int64_t window_ns = 0;
  std::int64_t settle_ns = 0;
  std::int64_t reached_ns = 0;  // the venue time the simulation has reached
  std::int64_t lag_ns = 0;      // the wall clock less reached_ns
  std::uint64_t events = 0;     // market data events so far, this session
  std::uint64_t late = 0;       // records later than settle_s (FollowSource::late): 0 is exact
  std::uint64_t seq_gaps = 0;
  ShadowStatusSide live;      // every window so far
  ShadowStatusSide sim;       // the live point, every window so far
  double net_error_lo = 0.0;  // 95% interval of sim.net - live.net; NaN until enough windows
  double net_error_hi = 0.0;
  ShadowStatusSide last_live;  // the last window closed
  ShadowStatusSide last_sim;
  std::uint32_t variants = 0;     // entries used, best first
  std::uint32_t instruments = 0;  // entries used, largest gap first
  ShadowStatusVariant variant[kShadowStatusVariants];
  ShadowStatusInstrument instrument[kShadowStatusInstruments];
};

[[nodiscard]] std::string default_shadow_status_path(const std::string& engine_name);

class ShadowStatusWriter {
 public:
  ShadowStatusWriter() = default;
  ~ShadowStatusWriter();
  ShadowStatusWriter(const ShadowStatusWriter&) = delete;
  ShadowStatusWriter& operator=(const ShadowStatusWriter&) = delete;
  // Creates or truncates the file; false fills `error`.
  [[nodiscard]] bool open(const std::string& path, std::string* error);
  void write(const ShadowStatus& s) noexcept;

 private:
  void* map_ = nullptr;
  std::size_t len_ = 0;
};

class ShadowStatusReader {
 public:
  ShadowStatusReader() = default;
  ~ShadowStatusReader();
  ShadowStatusReader(const ShadowStatusReader&) = delete;
  ShadowStatusReader& operator=(const ShadowStatusReader&) = delete;
  // False: no such file, or one of another build's layout.
  [[nodiscard]] bool open(const std::string& path);
  [[nodiscard]] bool is_open() const noexcept { return map_ != nullptr; }
  [[nodiscard]] bool read(ShadowStatus& out) const noexcept;

 private:
  const void* map_ = nullptr;
  std::size_t len_ = 0;
};

// The section fastmm-top draws under the session's status.
[[nodiscard]] std::string format_shadow_status(const ShadowStatus& s, std::int64_t now_ns);
[[nodiscard]] std::string shadow_status_json(const ShadowStatus& s);
// Prometheus text, appended to the session's by `fastmm-top --metrics`.
[[nodiscard]] std::string shadow_status_prometheus(const ShadowStatus& s, std::int64_t now_ns);

}  // namespace fastmm
