#pragma once
// Typed configuration (8.7). TOML parsing lives in src/core/config.cpp (toml++ is a
// private dependency of fastmm_core); this header exposes only plain structs.
//
// Sections: [engine] [venues.<x>] [venues.<x>.fees] [[instruments]] [strategy]
//           [strategy.params] [risk] [logging] [sim] [backtest] [storage]
// A [venues.<x>] key the generic parser does not read is kept verbatim in VenueSection::extra:
// the connector `kind` names owns it (include/fastmm/venues/registry.hpp).
// Rules: ${VAR} is substituted only inside [venues.*] strings; a value that looks like an
// inline secret (> 32 chars, no ${) is rejected unless allow_inline_secrets; redacted()
// prints the config with secrets masked; validation errors carry line:col.
#include "fastmm/core/enums.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/core/risk.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/strategies/params.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm {

inline constexpr std::size_t kMaxVenuesConfig = 8;  // == kMaxVenues (transport.hpp)

class ConfigError : public std::runtime_error {
 public:
  ConfigError(const std::string& msg, int line = 0, int col = 0)
      : std::runtime_error(line > 0 ? msg + " (line " + std::to_string(line) + ":" +
                                          std::to_string(col) + ")"
                                    : msg),
        line_(line),
        col_(col) {}
  [[nodiscard]] int line() const noexcept { return line_; }
  [[nodiscard]] int col() const noexcept { return col_; }

 private:
  int line_;
  int col_;
};

struct EngineSection {
  std::string name = "fastmm";
  int cpu = -1;
  std::vector<int> net_cpus;
  std::string spin_mode = "adaptive";
  // Carry the previous session's positions over from the store (venues that replay executions).
  bool restore_position = true;
  std::string net_backend = "epoll";  // net::Reactor backend: "epoll" | "io_uring"
  // fastmm-live: "split" runs the engine and each venue's network loop on their own threads;
  // "single" runs the one venue's network loop, the engine and order sending on the engine thread.
  std::string threading = "split";
  bool journal = true;
  std::string journal_dir = "runs";
  std::string journal_sync = "async";  // "async" | "fdatasync" (core/journal.hpp)
  std::size_t journal_max_bytes = 0;   // roll over to the next part at this size (0: never)
  int journal_retention_days = 0;      // delete .fmj files older than this at start (0: keep)
  std::string epoch_file = "runs/session_epoch";
  // Latched kill switch and cumulative PnL; empty = "<journal_dir>/<name>.kill".
  std::string kill_file;
  std::uint64_t rng_seed = 1;
  std::size_t md_ring_bytes = 4U << 20;
  std::size_t order_ring_bytes = 1U << 20;
  std::size_t journal_ring_bytes = 16U << 20;
  std::uint32_t max_events_per_step = 64;
  int crossed_grace_ms = 100;
  int ack_timeout_ms = 0;  // force-cancel an order without an ack after this long; 0 = off
  // Operator flatten (fastmm-ctl flatten): the sweep period, how long the engine keeps working the
  // position off (0 = until it is flat or an operator stops it) and the default slippage.
  int flatten_interval_ms = 500;
  int flatten_timeout_ms = 60000;
  int flatten_slippage_bps = 25;
  int latency_publish_ms = 1000;
  int tsc_recalibrate_s = 10;       // fastmm-live: TSC recalibration period, 0 = never
  std::int64_t timer_slack_ns = 0;  // fastmm-live: PR_SET_TIMERSLACK of its threads, 0 = kernel's
  bool lock_memory = false;         // fastmm-live: mlockall(MCL_CURRENT | MCL_FUTURE)
  int min_requote_ticks = 1;
  int min_requote_interval_ms = 50;
  int min_qty_bps = 8000;
  bool post_only = true;
  bool supports_replace = true;
  int reject_backoff_ms = 1000;       // QuoteManager: pause a side after a venue reject (0 = off)
  int reject_backoff_max_ms = 60000;  // doubling cap
  // fastmm-live after a kill switch it did not ask for (risk limit, internal failure, every venue
  // killed): "exit" shuts down like SIGTERM and exits with code 6; "stay" keeps running with
  // quoting off and logs an ERROR line every 10 s.
  std::string on_kill = "exit";
};

struct FeesSection {
  double maker_bps = 0.0;
  double taker_bps = 0.0;
};

struct VenueSection {
  std::string name;
  std::string kind;
  std::string ws_url;
  std::string ws_api_url;
  std::string rest_url;
  std::string api_key;
  std::string api_secret;
  bool testnet = true;
  bool supports_replace = false;
  bool insecure_tls = false;
  std::string ca_file;
  int recv_window_ms = 3000;
  FeesSection fees;
  // Every key the generic parser does not interpret, stringified. The venue `kind` names owns
  // them: it declares them, validates them and reports an unknown one
  // (fastmm::venues::validate_venues, include/fastmm/venues/registry.hpp), which is why the
  // central schema knows none of them. `extra_lines` is the line each key came from, absent for a
  // key set in code, so the owner can report with a position the way the schema does.
  std::map<std::string, std::string> extra;
  std::map<std::string, int> extra_lines;
};

struct InstrumentSection {
  std::string venue;
  std::string symbol;
  std::string base;
  std::string quote;
  std::string asset_class = "spot";
  std::string tick;
  std::string lot;
  std::string min_qty;
  std::string max_qty;
  std::string min_notional;
  std::string contract_multiplier = "1";
  bool enabled = true;
  int price_decimals = 8;
  std::string expiry;
  std::string strike;
  std::string option_type;
  // Per-instrument fee schedule; unset means the instrument pays its venue's [venues.<x>.fees].
  // Positive == a fee, negative == a rebate.
  std::optional<double> maker_bps;
  std::optional<double> taker_bps;
};

struct StrategySection {
  std::string name;
  ParamMap params;
  std::int64_t max_param_age_ms = 0;  // 0 = off
};

struct RiskSection {
  std::string max_order_qty;
  std::string max_order_notional;
  std::string max_position;
  int max_open_orders = 0;
  int price_collar_bps = 0;
  int fat_finger_bps = 0;
  int stale_md_ms = 0;
  std::string max_loss;
  std::string max_gross_notional;
  std::string max_net_notional;
  int orders_per_sec = 0;
  int burst = 0;
  bool stp = true;
};

struct LoggingSection {
  std::string level = "info";
  std::string file;
  std::string mirror_level = "warn";
};

// Free-form section used by the sim / backtest libraries: dotted keys -> stringified value.
struct GenericSection {
  std::map<std::string, std::string> values;
  std::map<std::string, int> lines;  // key -> line in the file; absent for keys set in code
  [[nodiscard]] bool has(std::string_view key) const { return values.contains(std::string(key)); }
  [[nodiscard]] std::string get_string(std::string_view key, std::string_view def = "") const;
  [[nodiscard]] std::int64_t get_int(std::string_view key, std::int64_t def = 0) const;
  [[nodiscard]] double get_double(std::string_view key, double def = 0.0) const;
  [[nodiscard]] bool get_bool(std::string_view key, bool def = false) const;
};

struct ConfigLoadOptions {
  bool allow_inline_secrets = false;
  bool substitute_env = true;
};

class Config {
 public:
  using LoadOptions = ConfigLoadOptions;

  static Config load(const std::string& path, const LoadOptions& opts = LoadOptions{});
  static Config parse(std::string_view toml,
                      const LoadOptions& opts = LoadOptions{},
                      std::string_view source_name = "<string>");

  EngineSection engine;
  std::vector<VenueSection> venues;  // VenueId == index
  std::vector<InstrumentSection> instruments;
  StrategySection strategy;
  RiskSection risk;
  LoggingSection logging;
  GenericSection sim;
  GenericSection backtest;
  // [storage]: the backend name and whatever keys that backend reads. Free-form, like [sim] and
  // [backtest]: a storage backend parses its own keys and the central schema knows none of them
  // (docs/reference/storage.md).
  GenericSection storage;
  std::vector<std::string> warnings;
  std::string source;      // path or "<string>"
  std::uint64_t hash = 0;  // FNV-1a of the raw text

  [[nodiscard]] const VenueSection* venue(std::string_view name) const noexcept;
  [[nodiscard]] VenueId venue_id(std::string_view name) const noexcept;  // invalid if unknown

  // Typed views used to wire the engine.
  [[nodiscard]] RiskLimits risk_limits() const;
  [[nodiscard]] QuoteParams quote_params() const;
  [[nodiscard]] LogLevel log_level() const;
  [[nodiscard]] LogLevel mirror_level() const;
  [[nodiscard]] SpinMode spin_mode() const;
  [[nodiscard]] bool single_threaded() const noexcept { return engine.threading == "single"; }

  // Dump with api_key/api_secret masked; safe to log.
  [[nodiscard]] std::string redacted() const;

  // The configuration as parsed (after any changes made to this object, e.g. command-line
  // overrides) as complete TOML without api_key / api_secret. Deterministic: equal
  // configurations give equal text, and parsing the text gives an equal configuration. Journals
  // embed it, and its hash is the journal header's config_hash.
  [[nodiscard]] std::string effective_toml() const;
  [[nodiscard]] std::uint64_t effective_hash() const { return text_hash(effective_toml()); }
  [[nodiscard]] static std::uint64_t text_hash(std::string_view text) noexcept;  // FNV-1a
};

// Builds the dense instrument table (src/core/instrument_loader.cpp). Throws ConfigError.
InstrumentTable load_instruments(const Config& cfg);

}  // namespace fastmm
