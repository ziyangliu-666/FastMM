// Every shipped configs/*.toml loads, passes the schema and configures its strategy through the
// registry, the way fastmm-backtest and fastmm-live apply [strategy.params]. The ${VAR} secrets a
// config references get placeholder values in this process only; nothing connects anywhere.
#include "test_support.hpp"

#include "fastmm/backtest/backtest_config.hpp"
#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/registrations.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/strategies/registry.hpp"

#if FASTMM_TUTORIAL_STRATEGIES
#include "strategies.hpp"  // examples/cpp/tutorial
#endif

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace fastmm;

namespace {

std::filesystem::path configs_dir() {
  return FASTMM_CONFIGS_DIR;
}

bool env_name_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

// The names of every ${NAME} in `text` (comments included: a placeholder costs nothing).
std::set<std::string> env_references(const std::string& text) {
  std::set<std::string> names;
  for (std::size_t i = text.find("${"); i != std::string::npos; i = text.find("${", i + 2)) {
    const std::size_t close = text.find('}', i + 2);
    if (close == std::string::npos) break;
    const std::string name = text.substr(i + 2, close - i - 2);
    if (!name.empty() && std::all_of(name.begin(), name.end(), env_name_char)) names.insert(name);
  }
  return names;
}

// Empty if `path` is a valid shipped configuration, otherwise the first error.
std::string check_config(const std::filesystem::path& path) {
  try {
    for (const std::string& name : env_references(fastmm::test::read_file(path)))
      ::setenv(name.c_str(), ("placeholder-" + name).c_str(), 1);
    const Config cfg = Config::load(path.string());
    if (!cfg.warnings.empty()) return "warning: " + cfg.warnings.front();
    const InstrumentTable instruments = load_instruments(cfg);
    if (cfg.strategy.name.empty()) {
      if (!cfg.strategy.params.empty()) return "[strategy.params] without [strategy] name";
      return {};  // e.g. configs/sim.toml, read by fastmm-sim-exchange
    }
    if (instruments.size() == 0) return "no [[instruments]] configured";
    bt::register_builtin_strategies();
#if FASTMM_TUTORIAL_STRATEGIES
    tutorial::register_strategies(StrategyRegistry::instance());
#endif
    if (StrategyRegistry::instance().find(cfg.strategy.name) == nullptr)
      return "unknown strategy '" + cfg.strategy.name + "'";
    // Build (but do not run) the engine runner: configure() parses every parameter and runs the
    // params struct's validate(); make_engine_runner throws std::invalid_argument on an error.
    bt::BacktestConfig b = bt::BacktestConfig::from_config(cfg);
    b.duration = seconds(1);
    b.output_dir.clear();
    b.journal_out.clear();
    bt::BacktestSession session(b, nullptr);
    const std::unique_ptr<IEngineRunner> runner =
        StrategyRegistry::instance().make(cfg.strategy.name, TransportKind::Sim, session.deps());
    if (runner == nullptr) return "strategy '" + cfg.strategy.name + "' has no sim runner";
  } catch (const std::exception& e) {
    return e.what();
  }
  return {};
}

// A copy of a shipped config whose first line starting with `prefix` is replaced by `line`.
std::filesystem::path mutated_copy(const std::string& config,
                                   const std::string& prefix,
                                   const std::string& line) {
  const std::string text = fastmm::test::read_file(configs_dir() / config);
  const std::size_t at = text.find("\n" + prefix);
  REQUIRE_MESSAGE(at != std::string::npos, config << " has no line starting with " << prefix);
  const std::size_t end = text.find('\n', at + 1);
  const std::string out = text.substr(0, at + 1) + line + text.substr(end);
  static int n = 0;
  const std::filesystem::path p =
      fastmm::test::tmp_dir() / ("mutated-" + std::to_string(++n) + "-" + config);
  std::ofstream(p, std::ios::binary) << out;
  return p;
}

}  // namespace

TEST_CASE("config.shipped: every configs/*.toml loads and configures its strategy") {
  std::vector<std::filesystem::path> files;
  for (const auto& e : std::filesystem::directory_iterator(configs_dir())) {
    if (e.is_regular_file() && e.path().extension() == ".toml") files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  REQUIRE(files.size() >= 9);
  for (const std::filesystem::path& f : files) {
    INFO("checking " << f.string());
    const std::string err = check_config(f);
    CHECK_MESSAGE(err.empty(), f.filename().string() << ": " << err);
  }
}

TEST_CASE("config.shipped: the check reports bad parameters and schema errors") {
  std::string err =
      check_config(mutated_copy("backtest-example.toml", "quote_qty = ", R"(quote_qty = "abc")"));
  CHECK_MESSAGE(err.find("quote_qty") != std::string::npos, err);
  err = check_config(mutated_copy("deribit-testnet.toml", "quote_qty = ", "quote_qty = -1"));
  CHECK_MESSAGE(err.find("quote_qty") != std::string::npos, err);
  err = check_config(mutated_copy("binance-testnet.toml", "levels = ", "levelz = 1"));
  CHECK_MESSAGE(err.find("unknown parameter 'levelz'") != std::string::npos, err);
  err = check_config(
      mutated_copy("bybit-testnet.toml", "max_open_orders = ", R"(max_open_orders = "four")"));
  CHECK_MESSAGE(err.find("max_open_orders") != std::string::npos, err);
  err = check_config(
      mutated_copy("sim-local.toml", R"(name = "basic_mm")", R"(name = "no_such_strategy")"));
  CHECK_MESSAGE(err.find("unknown strategy 'no_such_strategy'") != std::string::npos, err);
}

TEST_CASE("config: unknown [risk] and [backtest] keys are warnings with their line") {
  const Config cfg = Config::parse(R"([venues.sim]
kind = "sim"

[[instruments]]
venue = "sim"
symbol = "BTCUSDT"
tick = "0.01"
lot = "0.00001"

[risk]
max_postion = "0.05"

[backtest]
duraton_s = 5
duration_s = 5
)");
  const bt::BacktestConfig b = bt::BacktestConfig::from_config(cfg);
  REQUIRE(b.warnings.size() == 2);
  CHECK(b.warnings[0] == "unknown key 'risk.max_postion' ignored (line 11)");
  CHECK(b.warnings[1] == "unknown key 'backtest.duraton_s' ignored (line 14)");
  CHECK(b.duration == seconds(5));
}
