#pragma once
// Data-source registry: `--data <spec>` and `[backtest] source` resolve through a table of
// named openers, the same shape as the strategy registry. A source is one file plus a
// registration; it parses its own options and declares what it supports, so nothing about it
// reaches the central configuration schema (include/fastmm/config/schema.hpp).
//
//   spec := <name>[:<arg>[,<arg>...]]     arg := <key>=<value> | <positional>
//
//   synthetic                                     the market generator (opens to nullptr)
//   journal:tests/fixtures/journals/x.fmj         path is positional
//   csv:data.csv,venue=1
//   binance:BTCUSDT,2024-03-27,start=13:00,end=14:00
//
// A bare path with a known extension (.fmj, .csv) also works; open_data() falls back to it
// when the first segment names no registered source.
//
// Adding a source out of tree (docs/reference/data-sources.md):
//
//   std::unique_ptr<MdSource> open_mine(const DataSourceOptions& o) {
//     o.reject_unknown({"path", "venue"});
//     return std::make_unique<MySource>(o.require("path"));
//   }
//   DataSourceRegistry::instance().try_add({.name = "mine",
//                                           .summary = "my venue's tape",
//                                           .options = "path=<file>  venue=<id>",
//                                           .positional = {"path"},
//                                           .caps = {.top_of_book = true, .trades = true,
//                                                    .clock = "exchange time, nanoseconds"},
//                                           .open = &open_mine});
#include "fastmm/core/instrument.hpp"
#include "fastmm/sim/md_source.hpp"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fastmm::bt {

using sim::MdSource;

// What a source carries, so a reader of the run knows what the fills can mean. `depth` is L2
// beyond the touch; without it every order away from the best quote is modelled as alone at
// its price, because the feed never showed the queue there.
struct DataCapabilities {
  bool top_of_book = false;
  bool depth = false;
  bool trades = false;
  // One line naming the clock and its resolution, e.g. "venue transaction time, milliseconds".
  std::string_view clock;
};

// Parsed `<name>[:<arg>...]`. Values are views into the spec string the caller owns for the
// duration of the open() call.
class DataSourceOptions {
 public:
  // Throws std::runtime_error on a malformed spec (empty name, empty key, duplicate key).
  [[nodiscard]] static DataSourceOptions parse(std::string_view spec,
                                               std::span<const std::string_view> positional = {});

  [[nodiscard]] std::string_view name() const noexcept { return name_; }
  [[nodiscard]] std::string_view spec() const noexcept { return spec_; }
  [[nodiscard]] bool has(std::string_view key) const noexcept { return find(key) != nullptr; }
  [[nodiscard]] std::string_view get(std::string_view key,
                                     std::string_view fallback = {}) const noexcept {
    const std::string_view* v = find(key);
    return v == nullptr ? fallback : *v;
  }
  // Throws std::runtime_error when the key is missing or empty.
  [[nodiscard]] std::string_view require(std::string_view key) const;
  // Throws std::runtime_error when the value is not an integer / not true|false|1|0.
  [[nodiscard]] std::int64_t get_int(std::string_view key, std::int64_t fallback) const;
  [[nodiscard]] bool get_bool(std::string_view key, bool fallback) const;
  // Throws std::runtime_error naming the first key that is not in `known` (and the known set).
  void reject_unknown(std::initializer_list<std::string_view> known) const;

  // Instrument table of the run, when one exists: a source may map its symbols onto instrument
  // ids instead of taking `inst=`. Null for open_data() calls without a configuration.
  [[nodiscard]] const InstrumentTable* instruments() const noexcept { return instruments_; }
  void set_instruments(const InstrumentTable* t) noexcept { instruments_ = t; }

 private:
  [[nodiscard]] const std::string_view* find(std::string_view key) const noexcept;

  std::string_view spec_;
  std::string_view name_;
  std::vector<std::pair<std::string_view, std::string_view>> args_;
  const InstrumentTable* instruments_ = nullptr;
};

struct DataSourceEntry {
  // Returns nullptr for the synthetic market (the runner couples its generator to the venue).
  // Throws std::runtime_error with a message naming the source on bad options / missing files.
  using Opener = std::unique_ptr<MdSource> (*)(const DataSourceOptions&);

  std::string_view name;                     // spec prefix; must outlive the registry
  std::string_view summary;                  // one line, printed by `fastmm-data list`
  std::string_view options;                  // `key=<what>` list, printed by `fastmm-data list`
  std::vector<std::string_view> positional;  // option names an unnamed argument fills, in order
  DataCapabilities caps;
  Opener open = nullptr;
};

// Registry the CLIs, open_data(), open_source() and Python resolve `--data` through. Not
// thread-safe for writers: register before starting threads.
class DataSourceRegistry {
 public:
  static DataSourceRegistry& instance();

  // False when `name` is empty or already registered with a different opener; registering the
  // same opener again succeeds and changes nothing.
  bool try_add(DataSourceEntry entry);
  [[nodiscard]] const DataSourceEntry* find(std::string_view name) const noexcept;
  [[nodiscard]] const std::vector<DataSourceEntry>& entries() const noexcept { return entries_; }

 private:
  std::vector<DataSourceEntry> entries_;
};

// Registers synthetic, journal, csv and binance. Idempotent; every entry point calls it.
void register_builtin_data_sources();

// `fastmm-data list`: one block per registered source.
[[nodiscard]] std::string format_data_sources();

// Writes every event of `spec` to `out` as an .fmj journal (docs/reference/journal-format.md),
// the format the backtest replays fastest: the events are already decoded, so a replay does no
// text parsing. Returns the number written. Throws std::runtime_error on a bad spec or an
// unwritable file.
std::uint64_t convert_data(std::string_view spec,
                           const std::string& out,
                           const InstrumentTable& instruments,
                           std::uint64_t seed = 1);

}  // namespace fastmm::bt
