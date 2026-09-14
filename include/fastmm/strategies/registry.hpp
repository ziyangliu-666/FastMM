#pragma once
// Strategy registry (5.6, 8.6): name -> parameter schema + one factory per transport kind,
// each producing an IEngineRunner. The library that knows a backend registers that backend's
// factory (the backtest library registers Sim and Replay, a live app registers Live), so core
// never instantiates Engine<...> for backends it does not know about, and two libraries can
// register the same strategy name for different transports without one silently replacing the
// other:
//
//   std::unique_ptr<IEngineRunner> make_basic_mm(TransportKind k, RunnerDeps& d) { ... }
//   registry.add(BasicMM::name(), &BasicMM::schema(), TransportKind::Sim, make_basic_mm);
//
// make_engine_runner<S>() is the helper the factories use once they have built the
// clock/transport/feed for the requested backend.
#include "fastmm/core/engine.hpp"
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/strategies/params.hpp"
#include "fastmm/strategies/strategy.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace fastmm {

enum class TransportKind : std::uint8_t { Sim = 0, Live = 1, Replay = 2, Count = 3 };
[[nodiscard]] constexpr std::string_view to_string(TransportKind k) noexcept {
  switch (k) {
    case TransportKind::Sim:
      return "sim";
    case TransportKind::Live:
      return "live";
    case TransportKind::Replay:
      return "replay";
    case TransportKind::Count:
      return "count";
  }
  return "?";
}

// Everything a factory needs that is backend-independent. `backend` points at the
// app-provided bundle (SimBackend / LiveBackend) holding clock, transport and feed.
struct RunnerDeps {
  EngineConfig engine;
  const InstrumentTable* instruments = nullptr;
  ParamMap params;
  MsgRing* journal_ring = nullptr;
  void* backend = nullptr;
};

inline constexpr std::size_t kTransportKinds = static_cast<std::size_t>(TransportKind::Count);

struct StrategyEntry {
  using Factory = std::unique_ptr<IEngineRunner> (*)(TransportKind, RunnerDeps&);
  std::string_view name;
  const ParamSchema* schema = nullptr;
  std::array<Factory, kTransportKinds> factories{};  // indexed by TransportKind

  [[nodiscard]] bool supports(TransportKind k) const noexcept {
    const auto i = static_cast<std::size_t>(k);
    return i < kTransportKinds && factories[i] != nullptr;
  }
};

class StrategyRegistry {
 public:
  static StrategyRegistry& instance() {
    static StrategyRegistry r;
    return r;
  }
  // Registers `factory` for one transport kind. Returns false (and changes nothing) when the
  // factory is null, the kind is invalid, that kind is already registered for `name`, or `name`
  // is already registered with a different parameter schema.
  bool add(std::string_view name,
           const ParamSchema* schema,
           TransportKind kind,
           StrategyEntry::Factory factory) {
    const auto k = static_cast<std::size_t>(kind);
    if (factory == nullptr || k >= kTransportKinds || schema == nullptr) return false;
    for (auto& e : entries_) {
      if (e.name != name) continue;
      if (e.schema != schema || e.factories[k] != nullptr) return false;
      e.factories[k] = factory;
      return true;
    }
    StrategyEntry e{name, schema, {}};
    e.factories[k] = factory;
    entries_.push_back(e);
    return true;
  }
  [[nodiscard]] const StrategyEntry* find(std::string_view name) const noexcept {
    for (const auto& e : entries_) {
      if (e.name == name) return &e;
    }
    return nullptr;
  }
  [[nodiscard]] const std::vector<StrategyEntry>& entries() const noexcept { return entries_; }
  [[nodiscard]] std::unique_ptr<IEngineRunner> make(std::string_view name,
                                                    TransportKind kind,
                                                    RunnerDeps& deps) const {
    const StrategyEntry* e = find(name);
    if (e == nullptr || !e->supports(kind)) return nullptr;
    return e->factories[static_cast<std::size_t>(kind)](kind, deps);
  }

 private:
  std::vector<StrategyEntry> entries_;
};

[[nodiscard]] inline const std::vector<StrategyEntry>& list_strategies() {
  return StrategyRegistry::instance().entries();
}

// Builds strategy + engine for a concrete backend triple. Throws std::invalid_argument on
// bad parameters (startup only).
template <StrategyLike S, ClockLike Clock, TransportLike Transport, FeedLike Feed>
std::unique_ptr<IEngineRunner> make_engine_runner(RunnerDeps& deps,
                                                  Clock& clock,
                                                  Transport& transport,
                                                  Feed& feed) {
  using E = Engine<S, Clock, Transport, Feed>;
  auto strategy = std::make_unique<S>();
  if (auto err = strategy->configure(deps.params))
    throw std::invalid_argument(std::string(S::name()) + ": " + *err);
  auto engine = std::make_unique<E>(
      deps.engine, *deps.instruments, clock, transport, feed, *strategy, deps.journal_ring);
  return std::make_unique<EngineRunner<E, S>>(std::move(strategy), std::move(engine));
}

}  // namespace fastmm
