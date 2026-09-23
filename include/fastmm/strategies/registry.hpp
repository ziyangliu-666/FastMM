#pragma once
// Strategy registry (8.6, ADR-0012): name -> parameter schema + one factory per transport kind,
// each producing an IEngineRunner. Strategies are added by a registration function
// (strategies/module.hpp):
//
//   void mm::register_strategies(fastmm::StrategyRegistry& r) {
//     fastmm::register_strategy<mm::MicropriceMM>(r);   // Sim + Replay + Live factories
//   }
//
// Nothing registers itself: apps pass their registration functions to fastmm::cli::live /
// backtest / replay, and the built-in strategies come from fastmm::register_builtin_strategies().
// This header does not parse the Engine template; make_engine_runner<S>() lives in
// strategies/engine_factory.hpp.
#include "fastmm/core/engine_config.hpp"
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/strategies/params.hpp"
#include "fastmm/strategies/strategy.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
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

// Everything a factory needs that is backend-independent. `backend` points at the bundle the
// runtime provides for the requested kind (sim::SimBackend, sim::ReplayBackend, live::LiveBackend)
// holding clock, transport and feed.
struct RunnerDeps {
  EngineConfig engine;
  const InstrumentTable* instruments = nullptr;
  ParamMap params;
  MsgRing* journal_ring = nullptr;
  // Where the engine hands fills, orders, positions and kill events for a storage backend
  // (core/record_stream.hpp); null disables them at no cost.
  MsgRing* record_ring = nullptr;
  void* backend = nullptr;
};

inline constexpr std::size_t kTransportKinds = static_cast<std::size_t>(TransportKind::Count);

struct StrategyEntry {
  using Factory = std::unique_ptr<IEngineRunner> (*)(TransportKind, RunnerDeps&);
  std::string_view name;  // must outlive the registry (S::name() returns a literal)
  const ParamSchema* schema = nullptr;
  std::array<Factory, kTransportKinds> factories{};  // indexed by TransportKind

  [[nodiscard]] bool supports(TransportKind k) const noexcept {
    const auto i = static_cast<std::size_t>(k);
    return i < kTransportKinds && factories[i] != nullptr;
  }
};

enum class AddResult : std::uint8_t {
  Added,           // the factory is now registered for (name, kind)
  AlreadyPresent,  // the same schema and factory were registered before: nothing changed
  Conflict,  // `name` is registered with a different schema, or (name, kind) with another factory
  Invalid,   // empty name, null schema or factory, or an invalid kind
};
[[nodiscard]] constexpr std::string_view to_string(AddResult r) noexcept {
  switch (r) {
    case AddResult::Added:
      return "added";
    case AddResult::AlreadyPresent:
      return "already present";
    case AddResult::Conflict:
      return "conflict";
    case AddResult::Invalid:
      return "invalid";
  }
  return "?";
}

class StrategyRegistry {
 public:
  // The registry the CLIs, run_backtest(cfg, name), replay_journal and Python use. Not
  // thread-safe for writers: register before starting threads. Tests use a local registry.
  static StrategyRegistry& instance() {
    static StrategyRegistry r;
    return r;
  }

  // Registers `factory` for one transport kind of `name`. Registering the same schema and factory
  // again does nothing (AlreadyPresent); anything that would replace or disagree with an existing
  // registration changes nothing and returns Conflict.
  AddResult try_add(std::string_view name,
                    const ParamSchema* schema,
                    TransportKind kind,
                    StrategyEntry::Factory factory) {
    const auto k = static_cast<std::size_t>(kind);
    if (name.empty() || factory == nullptr || k >= kTransportKinds || schema == nullptr)
      return AddResult::Invalid;
    for (StrategyEntry& e : entries_) {
      if (e.name != name) continue;
      if (e.schema != schema) return AddResult::Conflict;
      if (e.factories[k] == factory) return AddResult::AlreadyPresent;
      if (e.factories[k] != nullptr) return AddResult::Conflict;
      e.factories[k] = factory;
      return AddResult::Added;
    }
    StrategyEntry e{name, schema, {}};
    e.factories[k] = factory;
    entries_.push_back(e);
    return AddResult::Added;
  }
  [[nodiscard]] const StrategyEntry* find(std::string_view name) const noexcept {
    for (const StrategyEntry& e : entries_) {
      if (e.name == name) return &e;
    }
    return nullptr;
  }
  [[nodiscard]] const std::vector<StrategyEntry>& entries() const noexcept { return entries_; }
  // Builds a runner; null when `name` or its factory for `kind` is missing, or the factory refuses
  // (wrong backend). Throws std::invalid_argument on bad parameters (startup only).
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

}  // namespace fastmm
