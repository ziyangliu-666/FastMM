#pragma once
// Strategy registry (5.6, 8.6): name -> schema + factory producing an IEngineRunner for a
// given transport kind. Registrations live in the apps (one .cpp per strategy) so the
// core library never instantiates Engine<...> for backends it does not know about:
//
//   std::unique_ptr<IEngineRunner> make_basic_mm(TransportKind k, RunnerDeps& d) { ... }
//   FASTMM_REGISTER_STRATEGY(BasicMM, make_basic_mm);
//
// make_engine_runner<S>() is the helper the factories use once they have built the
// clock/transport/feed for the requested backend.
#include "fastmm/core/engine.hpp"
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/strategies/params.hpp"
#include "fastmm/strategies/strategy.hpp"

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

struct StrategyEntry {
  using Factory = std::unique_ptr<IEngineRunner> (*)(TransportKind, RunnerDeps&);
  std::string_view name;
  const ParamSchema* schema;
  Factory factory;
};

class StrategyRegistry {
 public:
  static StrategyRegistry& instance() {
    static StrategyRegistry r;
    return r;
  }
  bool add(const StrategyEntry& e) {
    if (find(e.name) != nullptr) return false;
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
    return e == nullptr ? nullptr : e->factory(kind, deps);
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

#define FASTMM_REGISTER_STRATEGY(S, FactoryFn)                                   \
  namespace {                                                                    \
  const bool fastmm_registered_##S = ::fastmm::StrategyRegistry::instance().add( \
      ::fastmm::StrategyEntry{S::name(), &S::schema(), FactoryFn});              \
  }
