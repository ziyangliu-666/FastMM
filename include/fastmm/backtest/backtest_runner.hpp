#pragma once
// BacktestRunner (8.4): wires Engine<S, SimClock, SimTransport, InlineFeed> to a SimDriver
// and collects a BacktestResult. Two entry points share one BacktestSession:
//
//   run_backtest<MyStrategy>(cfg, source)        // template: no registration needed
//   run_backtest(cfg, "basic_mm", source)         // registry: apps, Python, sweeps
//
// `source == nullptr` selects the synthetic market: with the matching fill model the
// MarketGenerator is coupled to the venue (strategy orders rest in the same book as the
// synthetic flow); with the L2 queue model a SyntheticSource stream is replayed exactly like
// historical data. A non-null source is read from its current position (reset() it first to
// reuse one).
#include "fastmm/backtest/backtest_config.hpp"
#include "fastmm/backtest/data_source.hpp"
#include "fastmm/backtest/result.hpp"
#include "fastmm/backtest/synthetic_source.hpp"
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/sim/sim_backend.hpp"
#include "fastmm/strategies/registry.hpp"

#include <memory>
#include <string_view>

namespace fastmm::bt {

class BacktestSession {
 public:
  // `schema`: the strategy's parameters, recorded in cfg.journal_out (may be null).
  // `strategy_meta`: `key=value` lines recorded in cfg.journal_out (Python strategies).
  BacktestSession(const BacktestConfig& cfg,
                  MdSource* source,
                  const ParamSchema* schema = nullptr,
                  std::string_view strategy_meta = {});
  ~BacktestSession();
  BacktestSession(const BacktestSession&) = delete;
  BacktestSession& operator=(const BacktestSession&) = delete;

  [[nodiscard]] RunnerDeps& deps() noexcept { return deps_; }
  [[nodiscard]] sim::SimBackend& backend() noexcept { return *backend_; }
  // Parameter updates delivered at simulated times during run() (not owned; may be null).
  void set_param_schedule(sim::ParamSchedule* schedule) noexcept { params_ = schedule; }
  // Code run at simulated times during run() (sim::SimDriver::set_slow_hooks).
  void set_slow_hooks(const sim::SlowHooks& hooks) noexcept { slow_ = hooks; }
  // Runs to the end of the data / horizon. `runner` (may be null) supplies engine stats.
  BacktestResult run(const sim::EngineHooks& hooks,
                     const IEngineRunner* runner,
                     std::string_view strategy_name);

 private:
  struct Impl;
  BacktestConfig cfg_;
  MdSource* source_;
  std::unique_ptr<SyntheticSource> synthetic_;
  std::unique_ptr<sim::MarketGenerator> generator_;
  std::unique_ptr<sim::SimBackend> backend_;
  std::unique_ptr<Impl> impl_;
  RunnerDeps deps_;
  sim::ParamSchedule* params_ = nullptr;
  sim::SlowHooks slow_{};
};

template <StrategyLike S>
BacktestResult run_backtest(const BacktestConfig& cfg, MdSource* source = nullptr) {
  BacktestSession session(cfg, source, &S::schema());
  std::unique_ptr<IEngineRunner> runner = session.backend().template make_runner<S>(session.deps());
  return session.run(session.backend().hooks, runner.get(), S::name());
}

// Registry path (registers the built-in strategies first). Throws std::invalid_argument for
// an unknown strategy or bad parameters.
BacktestResult run_backtest(const BacktestConfig& cfg,
                            std::string_view strategy,
                            MdSource* source = nullptr);

// Convenience wrapper that owns the config.
template <StrategyLike S>
class BacktestRunner {
 public:
  explicit BacktestRunner(BacktestConfig cfg) : cfg_(std::move(cfg)) {}
  [[nodiscard]] BacktestConfig& config() noexcept { return cfg_; }
  [[nodiscard]] const BacktestConfig& config() const noexcept { return cfg_; }
  BacktestResult run(MdSource* source = nullptr) { return run_backtest<S>(cfg_, source); }

 private:
  BacktestConfig cfg_;
};

// The SyntheticSource a run with `source == nullptr` replays under the L2 queue fill model.
[[nodiscard]] SyntheticSourceConfig synthetic_source_config(const BacktestConfig& cfg);

// Resolves `--data` through the data-source registry (data_registry.hpp): "<name>:<args>",
// or a bare *.fmj / *.csv path. "synthetic" (and "") open to nullptr, which selects the market
// generator. `instruments` lets a source map its symbols onto instrument ids; it may be null.
// Throws std::runtime_error for an unknown source, bad options or unreadable files.
[[nodiscard]] std::unique_ptr<MdSource> open_data(std::string_view spec,
                                                  const InstrumentTable* instruments = nullptr);
// Source for [backtest] source / path. `source` is a whole spec ("binance:BTCUSDT:2024-03-27");
// a bare name plus `path` ("journal" + "x.fmj") is the same thing written as two keys.
[[nodiscard]] std::unique_ptr<MdSource> open_source(const BacktestConfig& cfg);

}  // namespace fastmm::bt
