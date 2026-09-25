// fastmm::cli::data: what market data a backtest can read, and how to pack it for replay.
//
//   fastmm-data list
//   fastmm-data convert --config configs/backtest-binance.toml
//       --data binance:BTCUSDT,2024-03-27 --out ~/.cache/fastmm/data/btcusdt-2024-03-27.fmj
//
// Downloading the files themselves is `python3 -m fastmm.data fetch` (no third-party packages).
//
// Exit codes: 0 ok, 2 bad command line, 3 bad config, 4 unreadable data, 5 write failure.
#include "command_line.hpp"

#include "fastmm/backtest/backtest_config.hpp"
#include "fastmm/backtest/data_registry.hpp"
#include "fastmm/cli/data.hpp"
#include "fastmm/cli/modules.hpp"
#include "fastmm/config/config.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>

namespace fastmm::cli {

namespace {

constexpr int kExitConfig = 3;
constexpr int kExitData = 4;
constexpr int kExitWrite = 5;

}  // namespace

int data(int argc, char** argv) {
  const std::string program = program_name(argc, argv, "fastmm-data");
  const char* prog = program.c_str();
  std::string spec;
  std::string config_path;
  std::string out;
  std::uint64_t seed = 1;

  CLI::App app("Lists the market-data sources a backtest can read and packs them into journals.",
               program);
  setup(app);
  app.footer("Downloading what a source reads: python3 -m fastmm.data fetch --help");
  // The options belong to convert but may come before it; `fallthrough` sends them here.
  const CLI::App* list = app.add_subcommand(
      "list", "registered data sources, their options and what each one carries");
  const CLI::App* convert = app.add_subcommand(
      "convert", "decode a source into an .fmj journal, the format a backtest replays fastest");
  for (CLI::App* sub : app.get_subcommands({})) sub->fallthrough();
  app.require_subcommand(0, 1);
  app.add_option("--data", spec, "convert: source, e.g. binance:BTCUSDT,2024-03-27")
      ->option_text("<spec>");
  app.add_option("--config", config_path, "convert: backtest config supplying the instruments")
      ->option_text("<file.toml>");
  app.add_option("--out", out, "convert: output journal")->option_text("<file.fmj>");
  app.add_option("--seed", seed, "convert: session id stamped in the journal (default 1)")
      ->option_text("<n>");
  if (const std::optional<int> rc = parse(app, argc, argv)) return *rc;

  if (!list->parsed() && !convert->parsed())
    return usage_error(app, "a command is required: list or convert");
  if (list->parsed()) {
    std::fputs(bt::format_data_sources().c_str(), stdout);
    return 0;
  }
  if (spec.empty() || out.empty() || config_path.empty())
    return usage_error(app, "convert needs --data, --config and --out");

  bt::BacktestConfig cfg;
  try {
    ConfigLoadOptions lo;
    lo.substitute_env = false;  // converting data needs no API keys
    cfg = bt::BacktestConfig::from_config(Config::load(config_path, lo));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: config error: %s\n", prog, e.what());
    return kExitConfig;
  }

  std::uint64_t events = 0;
  try {
    events = bt::convert_data(spec, out, cfg.instruments, seed);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", prog, e.what());
    return kExitData;
  }
  if (events == 0) {
    std::fprintf(stderr, "%s: no events written to %s\n", prog, out.c_str());
    return kExitWrite;
  }
  std::printf("%llu events -> %s\n", static_cast<unsigned long long>(events), out.c_str());
  return 0;
}

}  // namespace fastmm::cli
