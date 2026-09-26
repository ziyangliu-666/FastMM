// FxPlan: the currencies of an instrument table and the instruments that price them.
#include "fastmm/core/fx.hpp"

#include <fmt/format.h>

#include <optional>
#include <vector>

namespace fastmm {

namespace {

struct Resolved {
  std::string ccy;
  FxSource src;
};

int find_slot(const FxPlan& p, std::string_view ccy) noexcept {
  for (std::uint8_t i = 0; i < p.count; ++i) {
    if (p.names[i].view() == ccy) return i;
  }
  return -1;
}

}  // namespace

Result<FxPlan, std::string> build_fx_plan(const InstrumentTable& table,
                                          const AccountingSpec& spec,
                                          std::span<const std::string> venue_names,
                                          bool all_instruments) {
  FxPlan p;
  if (!spec.configured()) return p;
  const std::string& rep = spec.reporting_currency;
  if (rep.size() > Currency::kCapacity)
    return fail(fmt::format("[accounting] reporting_currency '{}' is longer than {} characters",
                            rep,
                            Currency::kCapacity));
  p.names[0] = Currency(rep);
  p.count = 1;

  std::vector<Resolved> sources;
  for (const auto& [ccy, where] : spec.fx) {
    const std::size_t colon = where.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == where.size())
      return fail(fmt::format(R"([accounting.fx] {} = "{}": expected "venue:symbol")", ccy, where));
    const std::string_view venue = std::string_view(where).substr(0, colon);
    const std::string_view symbol = std::string_view(where).substr(colon + 1);
    const Instrument* inst = nullptr;
    for (std::size_t v = 0; v < venue_names.size() && inst == nullptr; ++v) {
      if (venue_names[v] == venue) inst = table.find(VenueId{static_cast<std::uint8_t>(v)}, symbol);
    }
    if (inst == nullptr)
      return fail(fmt::format(
          "[accounting.fx] {} = \"{}\": {} is not an instrument of venue {} in this session; the "
          "source must be one the session subscribes to (list it in [[instruments]], with enabled "
          "= false if it is not traded)",
          ccy,
          where,
          symbol,
          venue));
    FxSource src{inst->id, false};
    if (inst->base.view() == ccy && inst->quote.view() == rep) {
      src.invert = false;
    } else if (inst->base.view() == rep && inst->quote.view() == ccy) {
      src.invert = true;
    } else {
      return fail(fmt::format(
          "[accounting.fx] {} = \"{}\": {} is {}/{}; a source prices {} in {} ({}/{}, or {}/{})",
          ccy,
          where,
          symbol,
          inst->base.view().empty() ? "?" : inst->base.view(),
          inst->quote.view().empty() ? "?" : inst->quote.view(),
          ccy,
          rep,
          ccy,
          rep,
          rep,
          ccy));
    }
    sources.push_back({ccy, src});
  }

  const auto source_of = [&](std::string_view ccy) -> std::optional<FxSource> {
    for (const Resolved& r : sources) {
      if (r.ccy == ccy) return r.src;
    }
    return std::nullopt;
  };

  for (const Instrument& inst : table) {
    const bool required = all_instruments || inst.enabled();
    const std::string_view sc = inst.settlement_ccy();
    if (sc.empty() && required)
      return fail(fmt::format(
          "{} names no settlement currency (its {} is empty), so [accounting] cannot convert it",
          inst.symbol.view(),
          inst.inverse() ? "base" : "quote"));
    int slot = find_slot(p, sc);
    if (slot < 0) {
      const std::optional<FxSource> src = source_of(sc);
      if (!src && required)
        return fail(fmt::format(
            "{} settles in {} and [accounting.fx] has no source for {}: add {} = \"venue:symbol\" "
            "naming an instrument that prices {} in {}",
            inst.symbol.view(),
            sc,
            sc,
            sc,
            sc,
            rep));
      if (p.count == kMaxCurrencies)
        return fail(
            fmt::format("[accounting]: more than {} settlement currencies", kMaxCurrencies));
      slot = p.count++;
      p.names[static_cast<std::size_t>(slot)] = Currency(sc);
      p.sources[static_cast<std::size_t>(slot)] = src.value_or(FxSource{});
    }
    p.ccy[inst.id.value] = static_cast<std::uint8_t>(slot);
  }
  for (std::uint8_t c = 1; c < p.count; ++c) {
    const FxSource& s = p.sources[c];
    if (s.instrument.valid()) p.prices[s.instrument.value] = static_cast<std::uint8_t>(c + 1);
  }
  return p;
}

Result<FxPlan, std::string> session_fx_plan(const InstrumentTable& table,
                                            const AccountingSpec& spec,
                                            std::span<const std::string> venue_names,
                                            bool all_instruments,
                                            bool guarded,
                                            std::string* warning) {
  Result<FxPlan, std::string> r = build_fx_plan(table, spec, venue_names, all_instruments);
  if (r || guarded) return r;
  if (warning != nullptr) *warning = r.error() + "; the PnL totals are not converted";
  return FxPlan{};
}

}  // namespace fastmm
