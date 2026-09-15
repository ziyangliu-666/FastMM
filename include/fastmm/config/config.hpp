#pragma once
// Typed configuration (8.7). TOML parsing lives in src/core/config.cpp (toml++ is a
// private dependency of fastmm_core); this header exposes only plain structs.
//
// Sections: [engine] [venues.<x>] [venues.<x>.fees] [[instruments]] [strategy]
//           [strategy.params] [risk] [logging] [sim] [backtest]
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
  std::string net_backend = "epoll";  // net::Reactor backend: "epoll" | "io_uring"
  bool journal = true;
  std::string journal_dir = "runs";
  std::string epoch_file = "runs/session_epoch";
  std::uint64_t rng_seed = 1;
  std::size_t md_ring_bytes = 4U << 20;
  std::size_t order_ring_bytes = 1U << 20;
  std::size_t journal_ring_bytes = 16U << 20;
  std::uint32_t max_events_per_step = 64;
  int crossed_grace_ms = 100;
  int latency_publish_ms = 1000;
  int tsc_recalibrate_s = 10;  // fastmm-live: TSC recalibration period, 0 = never
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
  std::map<std::string, std::string> extra;  // unknown keys, stringified
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
