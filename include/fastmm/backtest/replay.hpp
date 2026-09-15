#pragma once
// Deterministic replay (5.11): runs Engine<S, SimClock, ReplayTransport, JournalFeed> over a
// session journal recorded by fastmm-live, a sim or a backtest run (BacktestConfig::journal_out)
// and proves that the engine reproduces the recorded outbound stream bit for bit.
//
// A format v2 journal carries what a replay needs besides the events: the engine clock of every
// event, the session epoch, quoting enabled (dry run), the RNG seed, per-venue cancel-replace and
// the effective configuration. replay_journal(path) uses all of it; replay_journal(path, cfg)
// takes the engine and strategy configuration from `cfg` (what-if runs) and still restores the
// session settings from the header.
//
//   journal (inbound events + Out* copies) --JournalFeed--> engine --ReplayTransport--> sha256
//
// A replay passes when the replayed and recorded message counts and SHA-256 hashes are equal
// and no message differed (ReplayResult::ok()). A different strategy or parameters turn the
// same call into a what-if run (ok() is then expected to be false).
#include "fastmm/backtest/backtest_config.hpp"
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/strategies/params.hpp"
#include "fastmm/strategies/registry.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace fastmm::bt {

struct ReplayOptions {
  std::string strategy;  // empty -> the strategy recorded in the journal header
  bool verify = true;    // compare every outbound message with the journal's Out* records
  // Restore session epoch, quoting enabled, RNG seed and per-venue cancel-replace from a v2
  // header (a journal without them replays with the given configuration's values).
  bool session_from_journal = true;
  // Apply the journal's ParamUpdate records, their fields matched to the strategy's parameters by
  // name (false: skip them and keep the configured parameters, a what-if run).
  bool param_updates = true;
};

struct ReplayResult {
  std::string strategy;
  std::string outbound_sha256;  // what the replayed engine sent
  std::string recorded_sha256;  // the journal's Out* records
  std::uint64_t outbound_messages = 0;
  std::uint64_t recorded_messages = 0;
  std::uint64_t events = 0;          // inbound events replayed
  std::int64_t first_mismatch = -1;  // index of the first differing message (verify only)
  std::string expected_message;      // summaries at first_mismatch ("(none)" past the end)
  std::string actual_message;
  bool session_restored = false;  // the header's session settings were applied
  [[nodiscard]] bool ok() const noexcept {
    return first_mismatch < 0 && outbound_messages == recorded_messages &&
           outbound_sha256 == recorded_sha256;
  }
};

struct JournalInfo {
  std::string strategy;
  std::uint32_t version = 0;
  std::uint64_t rng_seed = 0;
  std::uint64_t config_hash = 0;
  std::int64_t start_ts = 0;
  std::uint64_t messages = 0;
  std::uint64_t outbound_messages = 0;  // > 0 == session journal, 0 == market data only
  std::uint64_t dropped_outbound = 0;   // of which the transport did not accept
  std::uint64_t market_data_messages = 0;
  bool has_session = false;  // v2 session settings below are valid
  std::uint16_t session_epoch = 0;
  bool quoting_enabled = true;
  std::uint64_t replace_venues = 0;
  bool engine_time = false;  // the events carry the engine clock
  std::string config_toml;   // embedded effective configuration (empty: none)
  std::string metadata;      // `key=value` lines about the strategy (empty: none)
};

// A strategy outside the registry (a Python hot strategy): the name for the result, the parameter
// schema that ParamUpdate fields are matched against, and a factory that builds the runner on the
// sim::ReplayBackend that `deps.backend` points at.
struct ReplayStrategy {
  std::string name;
  const ParamSchema* schema = nullptr;
  std::function<std::unique_ptr<IEngineRunner>(RunnerDeps& deps)> make;
};

// Throws std::runtime_error when the file cannot be opened / validated.
[[nodiscard]] JournalInfo inspect_journal(const std::string& path);

// The configuration embedded in the journal. Throws std::runtime_error when there is none (v1
// journals, market-data journals, hand-built backtest configurations) or it does not parse.
[[nodiscard]] BacktestConfig journal_config(const std::string& path);

// One-line description of an Out* message (type, ids, side, price, quantity, stamp).
[[nodiscard]] std::string describe_outbound(const EventHeader& h);

// `cfg` supplies the engine configuration and strategy parameters (it must match the
// recording run for a verifying replay); the instrument table is taken from the journal.
// Throws std::runtime_error / std::invalid_argument on unreadable journals or unknown
// strategies.
[[nodiscard]] ReplayResult replay_journal(const std::string& path,
                                          const BacktestConfig& cfg,
                                          const ReplayOptions& opt = {});
// Replays `strategy` instead of a registered one (opt.strategy is ignored).
[[nodiscard]] ReplayResult replay_journal(const std::string& path,
                                          const BacktestConfig& cfg,
                                          const ReplayOptions& opt,
                                          const ReplayStrategy& strategy);
// Replays with the journal's embedded configuration (journal_config(path)).
[[nodiscard]] ReplayResult replay_journal(const std::string& path, const ReplayOptions& opt = {});

}  // namespace fastmm::bt
