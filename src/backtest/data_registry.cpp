#include "fastmm/backtest/data_registry.hpp"

#include "fastmm/backtest/binance_source.hpp"
#include "fastmm/backtest/csv_source.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/tardis_source.hpp"

#include <cstdio>
#include <stdexcept>

namespace fastmm::bt {

namespace {

[[noreturn]] void bad(const std::string& what) {
  throw std::runtime_error("data: " + what);
}

std::string_view trim(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

}  // namespace

// ---- options ---------------------------------------------------------------------------------

DataSourceOptions DataSourceOptions::parse(std::string_view spec,
                                           std::span<const std::string_view> positional) {
  DataSourceOptions o;
  o.spec_ = spec;
  const std::size_t colon = spec.find(':');
  o.name_ = trim(spec.substr(0, colon));
  if (o.name_.empty()) bad("empty source name in '" + std::string(spec) + "'");
  std::string_view rest =
      colon == std::string_view::npos ? std::string_view{} : spec.substr(colon + 1);
  std::size_t at = 0;
  std::size_t next_positional = 0;
  while (at <= rest.size() && !rest.empty()) {
    const std::size_t comma = rest.find(',', at);
    const std::string_view seg = trim(
        rest.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at));
    at = comma == std::string_view::npos ? rest.size() + 1 : comma + 1;
    if (seg.empty()) continue;
    const std::size_t eq = seg.find('=');
    std::string_view key;
    std::string_view value;
    if (eq == std::string_view::npos) {
      if (next_positional >= positional.size()) {
        bad("'" + std::string(o.name_) + "': unexpected argument '" + std::string(seg) +
            "' (expected key=value)");
      }
      key = positional[next_positional++];
      value = seg;
    } else {
      key = trim(seg.substr(0, eq));
      value = trim(seg.substr(eq + 1));
      if (key.empty())
        bad("'" + std::string(o.name_) + "': empty option name in '" + std::string(seg) + "'");
    }
    if (o.has(key)) bad("'" + std::string(o.name_) + "': option '" + std::string(key) + "' twice");
    o.args_.emplace_back(key, value);
  }
  return o;
}

const std::string_view* DataSourceOptions::find(std::string_view key) const noexcept {
  for (const auto& [k, v] : args_) {
    if (k == key) return &v;
  }
  return nullptr;
}

std::string_view DataSourceOptions::require(std::string_view key) const {
  const std::string_view* v = find(key);
  if (v == nullptr || v->empty())
    bad("'" + std::string(name_) + "': option '" + std::string(key) + "' is required");
  return *v;
}

std::int64_t DataSourceOptions::get_int(std::string_view key, std::int64_t fallback) const {
  const std::string_view* v = find(key);
  if (v == nullptr || v->empty()) return fallback;
  std::int64_t out = 0;
  bool neg = false;
  std::string_view s = *v;
  if (s.front() == '-' || s.front() == '+') {
    neg = s.front() == '-';
    s.remove_prefix(1);
  }
  if (s.empty()) bad("'" + std::string(name_) + "': " + std::string(key) + " is not an integer");
  for (char c : s) {
    if (c < '0' || c > '9')
      bad("'" + std::string(name_) + "': " + std::string(key) + "='" + std::string(*v) +
          "' is not an integer");
    out = out * 10 + (c - '0');
  }
  return neg ? -out : out;
}

bool DataSourceOptions::get_bool(std::string_view key, bool fallback) const {
  const std::string_view* v = find(key);
  if (v == nullptr || v->empty()) return fallback;
  if (*v == "true" || *v == "1" || *v == "yes") return true;
  if (*v == "false" || *v == "0" || *v == "no") return false;
  bad("'" + std::string(name_) + "': " + std::string(key) + "='" + std::string(*v) +
      "' is not true or false");
}

void DataSourceOptions::reject_unknown(std::initializer_list<std::string_view> known) const {
  for (const auto& [k, v] : args_) {
    bool ok = false;
    for (std::string_view n : known) ok = ok || n == k;
    if (ok) continue;
    std::string msg = "'" + std::string(name_) + "': unknown option '" + std::string(k) + "'; try";
    for (std::string_view n : known) msg += " " + std::string(n);
    bad(msg);
  }
}

// ---- registry --------------------------------------------------------------------------------

DataSourceRegistry& DataSourceRegistry::instance() {
  static DataSourceRegistry r;
  return r;
}

bool DataSourceRegistry::try_add(DataSourceEntry entry) {
  if (entry.name.empty()) return false;
  for (const DataSourceEntry& e : entries_) {
    if (e.name == entry.name) return e.open == entry.open;
  }
  entries_.push_back(std::move(entry));
  return true;
}

const DataSourceEntry* DataSourceRegistry::find(std::string_view name) const noexcept {
  for (const DataSourceEntry& e : entries_) {
    if (e.name == name) return &e;
  }
  return nullptr;
}

// ---- built-in sources ------------------------------------------------------------------------

namespace {

std::unique_ptr<MdSource> open_synthetic(const DataSourceOptions& o) {
  o.reject_unknown({});
  return nullptr;  // the runner couples its generator to the venue
}

std::unique_ptr<MdSource> open_journal(const DataSourceOptions& o) {
  o.reject_unknown({"path"});
  return std::make_unique<JournalSource>(std::string(o.require("path")));
}

std::unique_ptr<MdSource> open_csv(const DataSourceOptions& o) {
  o.reject_unknown({"path", "venue"});
  const auto venue = static_cast<std::uint8_t>(o.get_int("venue", 0));
  return std::make_unique<CsvSource>(std::string(o.require("path")), VenueId{venue});
}

// Instrument id a source's rows carry: `inst=` wins, then the symbol in the configuration,
// then 0 (a single-instrument run, which is what a one-symbol dataset means).
InstrumentId resolve_instrument(const DataSourceOptions& o,
                                std::string_view source,
                                VenueId venue,
                                const std::string& symbol) {
  if (o.has("inst")) return InstrumentId{static_cast<std::uint32_t>(o.get_int("inst", 0))};
  if (o.instruments() == nullptr || o.instruments()->size() <= 1) return InstrumentId{0};
  const Instrument* inst = o.instruments()->find(venue, symbol);
  if (inst == nullptr) {
    bad("'" + std::string(source) + "': the configuration has no instrument '" + symbol +
        "' on venue " + std::to_string(venue.value) + "; add it or pass inst=<id>");
  }
  return inst->id;
}

std::unique_ptr<MdSource> open_binance(const DataSourceOptions& o) {
  o.reject_unknown({"symbol",
                    "date",
                    "from",
                    "to",
                    "start",
                    "end",
                    "market",
                    "dir",
                    "book",
                    "trades",
                    "clock",
                    "inst",
                    "venue"});
  BinanceSourceConfig cfg;
  cfg.symbol = std::string(o.require("symbol"));
  cfg.dir = std::string(o.get("dir", default_data_dir()));
  if (o.has("date")) {
    cfg.dates = date_range(o.get("date"), o.get("date"));
  } else if (o.has("from")) {
    cfg.dates = date_range(o.require("from"), o.get("to", o.get("from")));
  } else {
    bad("'binance': date=<YYYY-MM-DD>, or from=<date>,to=<date>, is required");
  }
  const std::string_view market = o.get("market", "um");
  const std::optional<BinanceMarket> m = parse_binance_market(market);
  if (!m) bad("'binance': market='" + std::string(market) + "' is not spot or um");
  cfg.market = *m;
  cfg.book = o.get_bool("book", cfg.market == BinanceMarket::UsdmFutures);
  cfg.trades = o.get_bool("trades", true);
  const std::string_view clock = o.get("clock", "transaction");
  if (clock == "event") {
    cfg.event_clock = true;
  } else if (clock != "transaction") {
    bad("'binance': clock='" + std::string(clock) + "' is not transaction or event");
  }
  if (o.has("start")) cfg.from = parse_utc_time(o.get("start"), cfg.dates.front());
  if (o.has("end")) cfg.to = parse_utc_time(o.get("end"), cfg.dates.back());
  cfg.venue = VenueId{static_cast<std::uint8_t>(o.get_int("venue", 0))};
  cfg.instrument = resolve_instrument(o, "binance", cfg.venue, cfg.symbol);
  return std::make_unique<BinanceDataset>(cfg);
}

std::unique_ptr<MdSource> open_tardis(const DataSourceOptions& o) {
  o.reject_unknown({"exchange",
                    "symbol",
                    "date",
                    "from",
                    "to",
                    "start",
                    "end",
                    "dir",
                    "book",
                    "trades",
                    "clock",
                    "inst",
                    "venue"});
  TardisSourceConfig cfg;
  cfg.exchange = std::string(o.require("exchange"));
  cfg.symbol = std::string(o.require("symbol"));
  cfg.dir = std::string(o.get("dir", default_data_dir()));
  if (o.has("date")) {
    cfg.dates = date_range(o.get("date"), o.get("date"));
  } else if (o.has("from")) {
    cfg.dates = date_range(o.require("from"), o.get("to", o.get("from")));
  } else {
    bad("'tardis': date=<YYYY-MM-DD>, or from=<date>,to=<date>, is required");
  }
  cfg.book = o.get_bool("book", true);
  cfg.trades = o.get_bool("trades", true);
  const std::string_view clock = o.get("clock", "exchange");
  if (clock == "local") {
    cfg.local_clock = true;
  } else if (clock != "exchange") {
    bad("'tardis': clock='" + std::string(clock) + "' is not exchange or local");
  }
  if (o.has("start")) cfg.from = parse_utc_time(o.get("start"), cfg.dates.front());
  if (o.has("end")) cfg.to = parse_utc_time(o.get("end"), cfg.dates.back());
  cfg.venue = VenueId{static_cast<std::uint8_t>(o.get_int("venue", 0))};
  cfg.instrument = resolve_instrument(o, "tardis", cfg.venue, cfg.symbol);
  return std::make_unique<TardisDataset>(cfg);
}

}  // namespace

void register_builtin_data_sources() {
  DataSourceRegistry& r = DataSourceRegistry::instance();
  r.try_add({.name = "synthetic",
             .summary = "the built-in market generator ([sim] in the configuration)",
             .options = "none",
             .positional = {},
             .caps = {.top_of_book = true,
                      .depth = true,
                      .trades = true,
                      .clock = "simulated, nanoseconds"},
             .open = &open_synthetic});
  r.try_add({.name = "journal",
             .summary = "the market data of a recorded .fmj session",
             .options = "path=<file.fmj>",
             .positional = {"path"},
             .caps = {.top_of_book = true,
                      .depth = true,
                      .trades = true,
                      .clock = "as recorded, nanoseconds"},
             .open = &open_journal});
  r.try_add({.name = "csv",
             .summary = "the ts_ns,type,inst,side,price,qty,seq row format",
             .options = "path=<file.csv>  venue=<id>",
             .positional = {"path"},
             .caps = {.top_of_book = true,
                      .depth = true,
                      .trades = true,
                      .clock = "as written, nanoseconds"},
             .open = &open_csv});
  r.try_add({.name = "binance",
             .summary = "data.binance.vision daily dumps (bookTicker + aggTrades)",
             .options = "symbol=<SYM>  date=<YYYY-MM-DD> | from=<date>,to=<date>  "
                        "start=<HH:MM>  end=<HH:MM>  market=spot|um  dir=<cache>  "
                        "book=<bool>  trades=<bool>  clock=transaction|event  inst=<id>  "
                        "venue=<id>",
             .positional = {"symbol", "date"},
             .caps = {.top_of_book = true,
                      .depth = false,
                      .trades = true,
                      .clock = "venue transaction time, milliseconds"},
             .open = &open_binance});
  r.try_add({.name = "tardis",
             .summary = "datasets.tardis.dev normalized L2 + trades (free: the 1st of a month)",
             .options = "exchange=<name>  symbol=<SYM>  date=<YYYY-MM-DD> | from=<date>,to=<date>  "
                        "start=<HH:MM>  end=<HH:MM>  dir=<cache>  book=<bool>  trades=<bool>  "
                        "clock=exchange|local  inst=<id>  venue=<id>",
             .positional = {"exchange", "symbol", "date"},
             .caps = {.top_of_book = true,
                      .depth = true,
                      .trades = true,
                      .clock = "exchange time, microseconds"},
             .open = &open_tardis});
}

std::uint64_t convert_data(std::string_view spec,
                           const std::string& out,
                           const InstrumentTable& instruments,
                           std::uint64_t seed) {
  register_builtin_data_sources();
  const std::size_t colon = spec.find(':');
  const DataSourceEntry* entry = DataSourceRegistry::instance().find(spec.substr(0, colon));
  if (entry == nullptr) bad("'" + std::string(spec) + "' names no registered source");
  DataSourceOptions opts = DataSourceOptions::parse(spec, entry->positional);
  opts.set_instruments(&instruments);
  std::unique_ptr<MdSource> source = entry->open(opts);
  if (source == nullptr) bad("'" + std::string(spec) + "' has no events to convert");
  return write_md_journal(*source, out, instruments, seed, "data");
}

namespace {
// Appends `text` under a "  <label>: " prefix, breaking at the double spaces that separate
// options so a long list stays inside 96 columns.
void append_wrapped(std::string& out, std::string_view label, std::string_view text) {
  const std::string indent(label.size() + 4, ' ');
  out.append("  ").append(label).append(": ");
  std::size_t column = indent.size();
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t stop = text.find("  ", at);
    if (stop == std::string_view::npos) stop = text.size();
    const std::string_view word = text.substr(at, stop - at);
    if (column > indent.size() && column + word.size() > 94) {
      out.append("\n").append(indent);
      column = indent.size();
    } else if (column > indent.size()) {
      out.append("  ");
      column += 2;
    }
    out.append(word);
    column += word.size();
    at = stop + 2;
  }
  out.append("\n");
}
}  // namespace

std::string format_data_sources() {
  register_builtin_data_sources();
  std::string out;
  for (const DataSourceEntry& e : DataSourceRegistry::instance().entries()) {
    out.append(e.name).append("\n  ").append(e.summary).append("\n");
    append_wrapped(out, "options", e.options);
    out.append("  carries: ");
    out.append(e.caps.top_of_book ? "top of book" : "no top of book");
    out.append(e.caps.depth ? ", L2 depth" : ", no depth beyond the touch");
    out.append(e.caps.trades ? ", trades" : ", no trades");
    out.append("\n  clock:   ").append(e.caps.clock).append("\n");
  }
  return out;
}

}  // namespace fastmm::bt
