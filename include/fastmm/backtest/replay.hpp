#pragma once
// Deterministic replay (5.11): runs Engine<S, SimClock, ReplayTransport, JournalFeed> over a
// session journal recorded by a sim/backtest run (BacktestConfig::journal_out) and proves
// that the engine reproduces the recorded outbound stream bit for bit.
//
//   journal (inbound events + Out* copies) --JournalFeed--> engine --ReplayTransport--> sha256
//
// A replay passes when the replayed and recorded message counts and SHA-256 hashes are equal
// and no message differed (ReplayResult::ok()). A different strategy or parameters turn the
// same call into a what-if run (ok() is then expected to be false).
#include "fastmm/backtest/backtest_config.hpp"

#include <cstdint>
#include <string>

namespace fastmm::bt {

struct ReplayOptions {
  std::string strategy;  // empty -> the strategy recorded in the journal header
  bool verify = true;    // compare every outbound message with the journal's Out* records
};

struct ReplayResult {
  std::string strategy;
  std::string outbound_sha256;  // what the replayed engine sent
  std::string recorded_sha256;  // the journal's Out* records
  std::uint64_t outbound_messages = 0;
  std::uint64_t recorded_messages = 0;
  std::uint64_t events = 0;          // inbound events replayed
  std::int64_t first_mismatch = -1;  // index of the first differing message (verify only)
  [[nodiscard]] bool ok() const noexcept {
    return first_mismatch < 0 && outbound_messages == recorded_messages &&
           outbound_sha256 == recorded_sha256;
  }
};

struct JournalInfo {
  std::string strategy;
  std::uint64_t rng_seed = 0;
  std::uint64_t config_hash = 0;
  std::int64_t start_ts = 0;
  std::uint64_t messages = 0;
  std::uint64_t outbound_messages = 0;  // > 0 == session journal, 0 == market data only
  std::uint64_t market_data_messages = 0;
};

// Throws std::runtime_error when the file cannot be opened / validated.
[[nodiscard]] JournalInfo inspect_journal(const std::string& path);

// `cfg` supplies the engine configuration and strategy parameters (it must match the
// recording run for a verifying replay); the instrument table is taken from the journal.
// Throws std::runtime_error / std::invalid_argument on unreadable journals or unknown
// strategies.
[[nodiscard]] ReplayResult replay_journal(const std::string& path,
                                          const BacktestConfig& cfg,
                                          const ReplayOptions& opt = {});

}  // namespace fastmm::bt
