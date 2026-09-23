// fastmm::cli::data: what market data a backtest can read, and how to pack it for replay.
//
//   fastmm-data list
//   fastmm-data convert --config configs/backtest-binance.toml
//       --data binance:BTCUSDT,2024-03-27 --out ~/.cache/fastmm/data/btcusdt-2024-03-27.fmj
//
// Downloading the files themselves is `python3 -m fastmm.data fetch` (no third-party packages).
//
// Exit codes: 0 ok, 2 bad command line, 3 bad config, 4 unreadable data, 5 write failure.
#include "fastmm/backtest/backtest_config.hpp"
#include "fastmm/backtest/data_registry.hpp"
#include "fastmm/cli/data.hpp"
#include "fastmm/cli/modules.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/version.hpp"

#include <cstdio>
#include <exception>
#include <string>
#include <string_view>

namespace fastmm::cli {

namespace {

constexpr int kExitUsage = 2;
constexpr int kExitConfig = 3;
constexpr int kExitData = 4;
constexpr int kExitWrite = 5;

void usage(std::FILE* out, const char* prog) {
  std::fprintf(out,
               "usage: %s <command> [options]\n"
               "  list                     registered data sources, their options and what\n"
               "                           each one carries\n"
               "  convert                  decode a source into an .fmj journal, the format a\n"
               "                           backtest replays fastest\n"
               "    --data <spec>          source, e.g. binance:BTCUSDT,2024-03-27\n"
               "    --config <file.toml>   backtest config supplying the instruments\n"
               "    --out <file.fmj>       output journal\n"
               "    --seed <n>             session id stamped in the journal (default 1)\n"
               "  --version | --help\n"
               "\n"
               "Downloading what a source reads: python3 -m fastmm.data fetch --help\n",
               prog);
}

}  // namespace

int data(int argc, char** argv) {
  const std::string program = program_name(argc, argv, "fastmm-data");
  const char* prog = program.c_str();
  std::string command;
  std::string spec;
  std::string config_path;
  std::string out;
  std::uint64_t seed = 1;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto value = [&](std::string& dst) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s: %s needs a value\n", prog, argv[i]);
        return false;
      }
      dst = argv[++i];
      return true;
    };
    if (a == "--help" || a == "-h") {
      usage(stdout, prog);
      return 0;
    } else if (a == "--version") {
      std::printf("%s %s\n", prog, build_info());
      return 0;
    } else if (a == "--data") {
      if (!value(spec)) return kExitUsage;
    } else if (a == "--config") {
      if (!value(config_path)) return kExitUsage;
    } else if (a == "--out") {
      if (!value(out)) return kExitUsage;
    } else if (a == "--seed") {
      std::string v;
      if (!value(v)) return kExitUsage;
      char* end = nullptr;
      seed = std::strtoull(v.c_str(), &end, 10);
      if (end == v.c_str() || *end != '\0') {
        std::fprintf(stderr, "%s: --seed expects an unsigned integer\n", prog);
        return kExitUsage;
      }
    } else if (!a.empty() && a.front() != '-' && command.empty()) {
      command = a;
    } else {
      std::fprintf(stderr, "%s: unknown argument '%s'\n", prog, argv[i]);
      usage(stderr, prog);
      return kExitUsage;
    }
  }

  if (command.empty()) {
    usage(stderr, prog);
    return kExitUsage;
  }
  if (command == "list") {
    std::fputs(bt::format_data_sources().c_str(), stdout);
    return 0;
  }
  if (command != "convert") {
    std::fprintf(stderr, "%s: unknown command '%s' (list | convert)\n", prog, command.c_str());
    return kExitUsage;
  }
  if (spec.empty() || out.empty() || config_path.empty()) {
    std::fprintf(stderr, "%s: convert needs --data, --config and --out\n", prog);
    return kExitUsage;
  }

  bt::BacktestConfig cfg;
  try {
    cfg = bt::BacktestConfig::from_config(Config::load(config_path));
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
