// Builds the InstrumentTable from [[instruments]] with exact decimal parsing.
#include "fastmm/config/config.hpp"

#include <fmt/format.h>

#include <cstdlib>
#include <ctime>

namespace fastmm {

namespace {

template <class F>
F decimal_or_throw(const std::string& s,
                   const InstrumentSection& i,
                   const char* what,
                   F def = F{}) {
  if (s.empty()) return def;
  const auto v = F::from_decimal(s);
  if (!v)
    throw ConfigError(
        fmt::format("instrument {}: {} '{}' is not a valid decimal", i.symbol, what, s));
  return *v;
}

AssetClass parse_asset_class(const std::string& s, const InstrumentSection& i) {
  if (s == "spot") return AssetClass::Spot;
  if (s == "perpetual" || s == "perp") return AssetClass::Perpetual;
  if (s == "future" || s == "futures") return AssetClass::Future;
  if (s == "option") return AssetClass::Option;
  if (s == "fx") return AssetClass::Fx;
  if (s == "equity") return AssetClass::Equity;
  throw ConfigError(fmt::format("instrument {}: unknown asset_class '{}'", i.symbol, s));
}

// "YYYY-MM-DD" or "YYYY-MM-DDTHH:MM:SSZ" -> ns since epoch (UTC).
std::int64_t parse_expiry(const std::string& s, const InstrumentSection& i) {
  if (s.empty()) return 0;
  tm t{};
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
  const int n = std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec);
  if (n < 3) throw ConfigError(fmt::format("instrument {}: bad expiry '{}'", i.symbol, s));
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  t.tm_sec = sec;
  const time_t secs = timegm(&t);
  return static_cast<std::int64_t>(secs) * 1'000'000'000;
}

}  // namespace

InstrumentTable load_instruments(const Config& cfg) {
  InstrumentTable table;
  for (const InstrumentSection& i : cfg.instruments) {
    Instrument inst{};
    const VenueId venue = cfg.venue_id(i.venue);
    if (!venue.valid())
      throw ConfigError(fmt::format("instrument {}: unknown venue '{}'", i.symbol, i.venue));
    inst.venue = venue;
    if (!inst.symbol.assign(i.symbol))
      throw ConfigError(fmt::format("instrument symbol '{}' is too long", i.symbol));
    static_cast<void>(inst.base.assign(i.base));
    static_cast<void>(inst.quote.assign(i.quote));
    inst.asset_class = parse_asset_class(i.asset_class, i);
    inst.flags = static_cast<std::uint8_t>(i.enabled ? Instrument::kEnabled : 0);
    inst.price_decimals = static_cast<std::uint8_t>(i.price_decimals);
    inst.tick = decimal_or_throw<Price>(i.tick, i, "tick");
    inst.lot = decimal_or_throw<Qty>(i.lot, i, "lot");
    inst.min_qty = decimal_or_throw<Qty>(i.min_qty, i, "min_qty", inst.lot);
    inst.max_qty = decimal_or_throw<Qty>(i.max_qty, i, "max_qty");
    inst.min_notional = decimal_or_throw<Notional>(i.min_notional, i, "min_notional");
    inst.contract_multiplier =
        decimal_or_throw<Qty>(i.contract_multiplier, i, "contract_multiplier", Qty::from_int(1));
    inst.expiry_ns = parse_expiry(i.expiry, i);
    inst.strike = decimal_or_throw<Price>(i.strike, i, "strike");
    if (i.option_type == "call") {
      inst.option_type = OptionType::Call;
    } else if (i.option_type == "put") {
      inst.option_type = OptionType::Put;
    } else if (!i.option_type.empty()) {
      throw ConfigError(fmt::format("instrument {}: option_type must be call|put", i.symbol));
    }
    if (!inst.tick.is_positive())
      throw ConfigError(fmt::format("instrument {}: tick must be > 0", i.symbol));
    if (!inst.lot.is_positive())
      throw ConfigError(fmt::format("instrument {}: lot must be > 0", i.symbol));
    if (!inst.contract_multiplier.is_positive()) {
      throw ConfigError(fmt::format("instrument {}: contract_multiplier must be > 0", i.symbol));
    }
    auto r = table.add(inst);
    if (!r) {
      const char* why = r.error() == InstrumentError::DuplicateSymbol ? "duplicate symbol"
                        : r.error() == InstrumentError::TableFull     ? "too many instruments"
                                                                      : "invalid instrument";
      throw ConfigError(fmt::format("instrument {}@{}: {}", i.symbol, i.venue, why));
    }
  }
  return table;
}

}  // namespace fastmm
