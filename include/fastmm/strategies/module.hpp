#pragma once
// Strategy modules (ADR-0012, section 5): one registration function per strategy library.
//
//   // mm/strategies.hpp
//   namespace mm { void register_strategies(fastmm::StrategyRegistry& r); }
//
//   // strategies.cpp: the only registration file a strategy library needs
//   #include "fastmm/strategies/factories.hpp"   // factory definitions: instantiates the engines
//   #include "mm/microprice_mm.hpp"
//   #include "mm/strategies.hpp"
//   void mm::register_strategies(fastmm::StrategyRegistry& r) {
//     fastmm::register_strategy<mm::MicropriceMM>(r);   // Sim + Replay + Live
//   }
//
//   // apps: int main(int argc, char** argv) { return fastmm::cli::live(argc, argv,
//   {mm::register_strategies}); }
//
// This header only declares the factories; it never instantiates Engine. Their definitions are in
// factory_sim.hpp, factory_replay.hpp and factory_live.hpp (factories.hpp includes all three). A
// file that calls register_strategy<S> without them only references the factories, so a missing
// instantiation is a link error naming the factory, not an "unknown strategy" at runtime. The app
// references the registration function, so the linker pulls it and the instantiations out of static
// archives: no static initialisers and no whole-archive linking.
//
// Only a strategy's owner instantiates it (two libraries instantiating the same factory break the
// one-definition rule); to use someone else's strategy, call their registration function.
#include "fastmm/strategies/registry.hpp"
#include "fastmm/strategies/strategy.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace fastmm {

// A strategy library's registration function.
using StrategyModule = void (*)(StrategyRegistry&);

// Thrown by register_strategy when a name is already registered with a different schema or factory.
class StrategyConflict : public std::logic_error {
 public:
  using std::logic_error::logic_error;
};

// Declared `template <class S>`, not `template <StrategyLike S>`: GCC and Clang mangle constrained
// templates differently. The definitions check StrategyLike<S> with a static_assert.
template <class S>
std::unique_ptr<IEngineRunner> sim_factory(TransportKind kind, RunnerDeps& deps);
template <class S>
std::unique_ptr<IEngineRunner> replay_factory(TransportKind kind, RunnerDeps& deps);
template <class S>
std::unique_ptr<IEngineRunner> live_factory(TransportKind kind, RunnerDeps& deps);

// Internal: the factories register_strategy adds. FastMM's tests use it; a strategy that should not
// run live is simply not registered by that project's live app.
enum class Transports : std::uint8_t { None = 0, Sim = 1, Replay = 2, Live = 4, All = 7 };
[[nodiscard]] constexpr Transports operator|(Transports a, Transports b) noexcept {
  return static_cast<Transports>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr bool has(Transports set, Transports t) noexcept {
  return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(t)) != 0;
}

template <class S>
void register_strategy(StrategyRegistry& r, Transports transports) {
  static_assert(StrategyLike<S>,
                "fastmm: register_strategy<S> needs a strategy: static name(), static schema() "
                "and a default constructor");
  const auto add = [&](TransportKind kind, StrategyEntry::Factory factory) {
    switch (r.try_add(S::name(), &S::schema(), kind, factory)) {
      case AddResult::Added:
      case AddResult::AlreadyPresent:
        return;
      case AddResult::Conflict:
        throw StrategyConflict("strategy '" + std::string(S::name()) + "' (" +
                               std::string(to_string(kind)) +
                               ") is already registered by different code; strategy names must be "
                               "unique across every registered library");
      case AddResult::Invalid:
        break;
    }
    throw std::invalid_argument("strategy '" + std::string(S::name()) +
                                "' cannot be registered: empty name or no parameter schema");
  };
  if (has(transports, Transports::Sim)) add(TransportKind::Sim, &sim_factory<S>);
  if (has(transports, Transports::Replay)) add(TransportKind::Replay, &replay_factory<S>);
  if (has(transports, Transports::Live)) add(TransportKind::Live, &live_factory<S>);
}

// Adds the Sim, Replay and Live factories of S. Registering the same strategy again does nothing;
// throws StrategyConflict when another strategy already uses the name.
template <class S>
void register_strategy(StrategyRegistry& r) {
  register_strategy<S>(r, Transports::All);
}

}  // namespace fastmm

// Explicit instantiation of one factory, for splitting a strategy's engines over several files:
//
//   // microprice_mm_live.cpp
//   #include "fastmm/strategies/factory_live.hpp"
//   #include "mm/microprice_mm.hpp"
//   FASTMM_INSTANTIATE_STRATEGY(mm::MicropriceMM, live);
//
// `kind` is sim, replay or live; the matching factory_<kind>.hpp must be included.
// (The trailing return type keeps `unique_ptr<...> ::fastmm::...` from parsing as a nested name.)
#define FASTMM_INSTANTIATE_STRATEGY(S, kind)                                                \
  template auto ::fastmm::kind##_factory<S>(::fastmm::TransportKind, ::fastmm::RunnerDeps&) \
      ->std::unique_ptr<::fastmm::IEngineRunner>
