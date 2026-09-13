// Built-in strategy registrations for the Sim / Replay transports (8.6). Deliberately no
// static initialisers: an object file in a static library is only linked when something
// references it, so apps, tests and the Python module call register_builtin_strategies()
// explicitly (run_backtest by name and replay_journal do it for you).
#include "fastmm/backtest/registrations.hpp"

#include "fastmm/sim/sim_backend.hpp"
#include "fastmm/strategies/avellaneda_stoikov.hpp"
#include "fastmm/strategies/basic_mm.hpp"
#include "fastmm/strategies/registry.hpp"

namespace fastmm::bt {

namespace {
std::unique_ptr<IEngineRunner> make_basic_mm(TransportKind k, RunnerDeps& d) {
  return sim::make_sim_or_replay_runner<BasicMM>(k, d);
}
std::unique_ptr<IEngineRunner> make_avellaneda_stoikov(TransportKind k, RunnerDeps& d) {
  return sim::make_sim_or_replay_runner<AvellanedaStoikov>(k, d);
}
}  // namespace

std::size_t register_builtin_strategies() {
  StrategyRegistry& r = StrategyRegistry::instance();
  // add() refuses duplicates, so repeated calls are harmless.
  for (const TransportKind k : {TransportKind::Sim, TransportKind::Replay}) {
    static_cast<void>(r.add(BasicMM::name(), &BasicMM::schema(), k, make_basic_mm));
    static_cast<void>(
        r.add(AvellanedaStoikov::name(), &AvellanedaStoikov::schema(), k, make_avellaneda_stoikov));
  }
  return r.entries().size();
}

}  // namespace fastmm::bt
