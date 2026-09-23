#pragma once
// State that survives a process restart: the session epoch (client order ids) and the latched risk
// state (kill switch and cumulative PnL). Both are text files next to the journal, written through
// a temporary file + fsync + rename, so a crash leaves the old file or the new one, never a
// truncated one. Both stores fail closed: a session that cannot read or bump them must not trade.
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace fastmm {

// Writes `contents` to `path` atomically: <path>.tmp, fsync, rename, fsync of the directory.
[[nodiscard]] Result<void, std::string> write_file_atomic(const std::string& path,
                                                          std::string_view contents);

// The 16-bit session epoch is the high half of every ClientOrderId, so two sessions that share one
// would let a venue's late message from the earlier session match an order of the later one.
class SessionEpochStore {
 public:
  // Reads the durable counter, increments it, writes it back and returns the epoch to use
  // (1..65535, never 0). Fails when the file cannot be read or written; the caller must not trade.
  // Beyond 65535 sessions the epoch cycles and `wrapped` is set: ids of sessions that long ago
  // can repeat.
  [[nodiscard]] static Result<std::uint16_t, std::string> next_epoch(const std::string& path,
                                                                     bool* wrapped = nullptr);
};

// Cumulative risk state across sessions. `realized` and `fees` are the sum over every session since
// the file was last cleared, so [risk] max_loss is a budget for the deployment rather than per
// process. Unrealized PnL is not carried; it is remeasured from the venue's position after the
// restart.
struct KillState {
  bool latched = false;  // a max-loss trip is latched: refuse to start until an operator clears it
  KillReason reason = KillReason::None;
  Notional realized{};
  Notional fees{};
  std::uint64_t sessions = 0;
  std::int64_t updated_ns = 0;  // wall clock of the last write

  // The carry [risk] max_loss is measured against, on top of the running session's PnL.
  [[nodiscard]] Notional carry() const noexcept { return realized - fees; }
};

class KillStateStore {
 public:
  // "<journal_dir>/<engine name>.kill".
  [[nodiscard]] static std::string default_path(std::string_view journal_dir,
                                                std::string_view engine_name);
  // A missing file is a cleared state; a file that cannot be read or parsed is an error.
  [[nodiscard]] static Result<KillState, std::string> load(const std::string& path);
  [[nodiscard]] static Result<void, std::string> store(const std::string& path, const KillState& s);
  // Removes the file (an operator clearing a latched trip and the loss budget with it).
  [[nodiscard]] static Result<void, std::string> clear(const std::string& path);
};

}  // namespace fastmm
