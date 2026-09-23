#pragma once
// Storage backend interface: what a session's fills, orders, positions, PnL and kill events are
// handed to, so they can be queried without replaying the journal.
//
//   engine thread --RecordWriter--> MsgRing --StoreThread--> Backend --> rows
//
// The journal stays the byte-exact stream a replay consumes (core/journal.hpp); a backend holds
// the interpreted record. Backends are selected by name from [storage] backend and built by the
// registry (store/registry.hpp); "sqlite" ships with FastMM and "none" means no backend at all.
//
// What a backend must guarantee (docs/reference/storage.md):
//   * Ordering. Records arrive in the order the engine made them, on one thread, and seq is
//     strictly increasing within a session. Calls between begin() and commit() are one batch.
//   * At most once. A record the ring dropped, or a batch the process died inside of, is gone; a
//     backend must not invent one. Records carry keys (session id + seq, and an exec id on a
//     fill) so re-ingesting the same record is a no-op.
//   * Never block the caller for long. StoreThread runs the backend on its own thread, so a slow
//     commit costs ring space, not engine latency, but a backend that blocks for seconds will
//     overflow the ring and lose records.
//   * Never throw across the interface, and never abort the process. A failure is counted in
//     errors() and described by last_error(); the session keeps trading.
//
// Everything here runs off the engine thread and may allocate.
#include "fastmm/config/config.hpp"
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/record_stream.hpp"
#include "fastmm/core/result.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::store {

// What a backend is built with. `config` is the [storage] section verbatim: a backend reads its
// own keys from it and the central schema (config/schema.hpp) knows none of them.
struct BackendOptions {
  const GenericSection* config = nullptr;
  std::string engine_name;  // [engine] name
  std::string default_dir;  // [engine] journal_dir; where a backend may default its files
  bool read_only = false;   // opened by a query tool, not by a session
};

// Written once, before any record.
struct SessionOpen {
  std::uint64_t session_id = 0;
  std::uint16_t session_epoch = 0;
  std::int64_t started_ns = 0;
  std::string engine_name;
  std::string strategy;
  std::string version;     // FASTMM_VERSION_STRING
  std::string build_info;  // compiler, flags, build type
  std::uint64_t config_hash = 0;
  std::string config_toml;   // effective configuration, secrets omitted
  std::string journal_path;  // first journal part; empty when journaling is off
  std::string host;
  std::uint32_t pid = 0;
  bool dry_run = false;
  std::int64_t pnl_carry_raw = 0;  // net PnL carried in from earlier sessions
};

// Written once, after the last record.
struct SessionClose {
  std::uint64_t session_id = 0;
  std::int64_t stopped_ns = 0;
  int exit_code = 0;
  KillReason kill_reason = KillReason::None;
  bool kill_latched = false;
  bool journal_complete = true;  // the journal was closed cleanly
  std::uint64_t journal_bytes = 0;
  std::vector<std::string> journal_paths;  // every part, in order
  RunnerStats stats;
};

class Backend {
 public:
  Backend() = default;
  virtual ~Backend() = default;
  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  // Opens or creates the store and brings its schema up to date. A failure here stops the
  // session from starting: an operator who does not want a store sets backend = "none".
  [[nodiscard]] virtual Result<void, std::string> open(const BackendOptions& opts) = 0;
  [[nodiscard]] virtual Result<void, std::string> session_open(const SessionOpen& s) = 0;
  // The instrument table of this session, by dense id.
  [[nodiscard]] virtual Result<void, std::string> instruments(
      std::uint64_t session_id, std::span<const Instrument> table) = 0;

  // One batch. StoreThread calls begin(), then zero or more records, then commit().
  virtual void begin() = 0;
  virtual void fill(const FillRecord& r) = 0;
  virtual void order(const OrderRecord& r) = 0;
  virtual void position(const PositionRecord& r) = 0;
  virtual void kill(const KillRecord& r) = 0;
  virtual void commit() = 0;

  [[nodiscard]] virtual Result<void, std::string> session_close(const SessionClose& s) = 0;
  virtual void close() = 0;

  [[nodiscard]] virtual std::uint64_t rows() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t errors() const noexcept = 0;
  [[nodiscard]] virtual std::string last_error() const = 0;
};

}  // namespace fastmm::store
