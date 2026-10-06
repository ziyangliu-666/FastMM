// fastmm::cli::data: what market data a backtest can read, and how to pack it for replay.
//
//   fastmm-data list
//   fastmm-data convert --config configs/backtest-binance.toml
//       --data binance:BTCUSDT,2024-03-27 --out ~/.cache/fastmm/data/btcusdt-2024-03-27.fmj
//   fastmm-data fill-check runs/demo-1/session.fmj [--conservatism 0,0.5,1] [--csv orders.csv]
//       [--horizons-ms 100,1000,10000] [--pre-ms 100] [--market recorded.fmj]
//   fastmm-data calibrate a.fmj b.fmj c.fmj [--conservatism ...] [--backtest [--config f.toml]]
//       [--csv grid.csv]
//
// Downloading the files themselves is `python3 -m fastmm.data fetch` (no third-party packages).
//
// Exit codes: 0 ok, 2 bad command line, 3 bad config, 4 unreadable data or journal, 5 write
// failure.
#include "command_line.hpp"

#include "fastmm/backtest/backtest_config.hpp"
#include "fastmm/backtest/calibrate.hpp"
#include "fastmm/backtest/data_registry.hpp"
#include "fastmm/backtest/fill_check.hpp"
#include "fastmm/cli/data.hpp"
#include "fastmm/cli/modules.hpp"
#include "fastmm/config/config.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

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
  std::vector<std::string> configs;
  std::string out;
  std::uint64_t seed = 1;
  std::string journal;
  std::vector<std::string> journals;
  std::vector<double> conservatism;
  std::string csv;
  bool backtest = false;
  std::vector<double> horizons_ms;
  double pre_ms = 100;
  std::string market;

  CLI::App app("Lists the market-data sources a backtest can read and packs them into journals.",
               program);
  setup(app);
  app.footer("Downloading what a source reads: python3 -m fastmm.data fetch --help");
  // The options belong to convert but may come before it; `fallthrough` sends them here.
  const CLI::App* list = app.add_subcommand(
      "list", "registered data sources, their options and what each one carries");
  const CLI::App* convert = app.add_subcommand(
      "convert", "decode a source into an .fmj journal, the format a backtest replays fastest");
  CLI::App* fill_check = app.add_subcommand(
      "fill-check",
      "how many of a live session's resting orders the l2_queue fill model would have filled");
  fill_check->add_option("journal", journal, "journal recorded by fastmm-live")
      ->option_text("<file.fmj>")
      ->required();
  fill_check
      ->add_option("--horizons-ms",
                   horizons_ms,
                   "markout horizons of the diagnosis, in ms (default 100,1000,10000)")
      ->option_text("<ms,...>")
      ->delimiter(',')
      ->check(CLI::PositiveNumber);
  fill_check
      ->add_option("--pre-ms", pre_ms, "the diagnosis's mid move before each fill, in ms (100)")
      ->option_text("<ms>")
      ->check(CLI::NonNegativeNumber);
  fill_check
      ->add_option("--market",
                   market,
                   "replay this journal's market data (the same period, instruments matched by "
                   "venue and symbol) instead of the session's own")
      ->option_text("<file.fmj>");
  CLI::App* calibrate = app.add_subcommand(
      "calibrate",
      "fit queue_conservatism and the latencies to live sessions, cross-validated, and print the "
      "[backtest] keys");
  calibrate->add_option("journals", journals, "journals recorded by fastmm-live")
      ->option_text("<file.fmj>...")
      ->required();
  calibrate->add_flag("--backtest",
                      backtest,
                      "also re-run each session as a strip_own backtest, as configured and with "
                      "the fitted keys, beside the live session");
  for (CLI::App* sub : app.get_subcommands({})) sub->fallthrough();
  app.require_subcommand(0, 1);
  app.add_option("--data", spec, "convert: source, e.g. binance:BTCUSDT,2024-03-27")
      ->option_text("<spec>");
  app.add_option(
         "--config",
         configs,
         "convert: backtest config supplying the instruments; calibrate --backtest: the "
         "configuration to run instead of the journals' own, one for all or one per journal")
      ->option_text("<file.toml>");
  app.add_option("--out", out, "convert: output journal")->option_text("<file.fmj>");
  app.add_option("--seed", seed, "convert: session id stamped in the journal (default 1)")
      ->option_text("<n>");
  app.add_option("--conservatism",
                 conservatism,
                 "fill-check, calibrate: queue_conservatism values to compare (default 0,0.5,1; "
                 "calibrate 0,0.25,0.5,0.75,1)")
      ->option_text("<c,...>")
      ->delimiter(',')
      ->check(CLI::Range(0.0, 1.0));
  app.add_option("--csv",
                 csv,
                 "fill-check: also write one row per order; calibrate: one row per session, "
                 "inputs and conservatism")
      ->option_text("<file.csv>");
  if (const std::optional<int> rc = parse(app, argc, argv)) return *rc;

  if (!list->parsed() && !convert->parsed() && !fill_check->parsed() && !calibrate->parsed())
    return usage_error(app, "a command is required: list, convert, fill-check or calibrate");
  const auto write_csv = [&](const std::string& text) {
    std::ofstream f(csv, std::ios::trunc);
    f << text;
    if (f) return true;
    std::fprintf(stderr, "%s: cannot write %s\n", prog, csv.c_str());
    return false;
  };
  if (calibrate->parsed()) {
    if (conservatism.empty()) conservatism = {0.0, 0.25, 0.5, 0.75, 1.0};
    bt::Calibration c;
    std::vector<bt::BacktestGap> gaps;
    try {
      c = bt::calibrate(journals, conservatism);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "%s: %s\n", prog, e.what());
      return kExitData;
    }
    std::fputs(bt::format_calibration(c).c_str(), stdout);
    std::fputs(("\n" + bt::calibration_snippet(c)).c_str(), stdout);
    if (!csv.empty() && !write_csv(bt::calibration_csv(c))) return kExitWrite;
    if (!backtest) return 0;
    if (configs.size() > 1 && configs.size() != journals.size())
      return usage_error(app, "calibrate --backtest takes one --config, or one per journal");
    try {
      gaps = bt::compare_backtests(c, configs);
    } catch (const ConfigError& e) {
      std::fprintf(stderr, "%s: config error: %s\n", prog, e.what());
      return kExitConfig;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "%s: %s\n", prog, e.what());
      return kExitData;
    }
    std::fputs(("\n" + bt::format_backtest_gaps(gaps)).c_str(), stdout);
    return 0;
  }
  if (list->parsed()) {
    std::fputs(bt::format_data_sources().c_str(), stdout);
    return 0;
  }
  if (fill_check->parsed()) {
    if (conservatism.empty()) conservatism = {0.0, 0.5, 1.0};
    bt::FillCheckOptions opt;
    if (!horizons_ms.empty()) {
      opt.horizons_ns.clear();
      for (const double h : horizons_ms) opt.horizons_ns.push_back(std::llround(h * 1e6));
    }
    opt.pre_window = Duration{std::llround(pre_ms * 1e6)};
    opt.market = market;
    bt::FillCheckResult r;
    try {
      r = bt::fill_check(journal, conservatism, opt);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "%s: %s\n", prog, e.what());
      return kExitData;
    }
    std::fputs(bt::format_fill_check(r).c_str(), stdout);
    // The diagnosis at the last value listed.
    std::fputs(("\n" + bt::format_fill_diagnosis(r, r.conservatism.size() - 1)).c_str(), stdout);
    if (!csv.empty() && !write_csv(bt::fill_check_csv(r))) return kExitWrite;
    return 0;
  }
  if (spec.empty() || out.empty() || configs.size() != 1)
    return usage_error(app, "convert needs --data, one --config and --out");
  const std::string& config_path = configs.front();

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
