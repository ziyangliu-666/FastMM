#pragma once
// The built-in strategies (basic_mm, avellaneda_stoikov, options_mm) as a strategy module, in the
// fastmm::strategies library. fastmm::cli::live, backtest and replay always register them first;
// run_backtest(cfg, name), replay_journal and sweep do so through
// bt::register_builtin_strategies().
namespace fastmm {

class StrategyRegistry;

// Registers the Sim, Replay and Live factories of every built-in strategy. Repeated calls are
// harmless; throws StrategyConflict if another strategy already took one of their names.
void register_builtin_strategies(StrategyRegistry& r);

}  // namespace fastmm
