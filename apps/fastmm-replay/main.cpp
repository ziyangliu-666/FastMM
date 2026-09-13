// fastmm-replay: deterministic replay proof (5.11).
//
//   fastmm-replay --journal session.fmj [--config cfg.toml] [--strategy name] [--verify]
//   fastmm-replay --journal tests/fixtures/journals/sample_1000.fmj --verify
//
// Session journal (recorded with outbound copies, e.g. fastmm-backtest --journal-out):
//   Engine<S, SimClock, ReplayTransport, JournalFeed> replays the inbound events; the
//   outbound SHA-256 and every message are compared with the recorded Out* records.
//   --strategy / --param-style what-ifs simply report the divergence.
// Market-data journal (no outbound records, e.g. the golden fixture):
//   the strategy is backtested on the journal (SimTransport), the run is recorded to
//   --out (a temporary file by default) and that session is replayed as above. With --verify
//   the backtest's outbound hash must also equal the sidecar <journal>.sha256 (or
//   --expect <hex>).
//
// Exit codes: 0 match (or no verification requested), 1 mismatch, 2 bad command line,
// 3 unreadable config / journal / unknown strategy.
#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/registrations.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/version.hpp"

#include <unistd.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

constexpr int kExitMismatch = 1;
constexpr int kExitUsage = 2;
constexpr int kExitInput = 3;

void usage(std::FILE* out) {
  std::fprintf(out,
               "usage: fastmm-replay --journal <in.fmj> [options]\n"
               "  --config <file.toml>  engine / strategy configuration of the recording\n"
               "                        (default: configs/backtest-example.toml if present)\n"
               "  --strategy <name>     strategy to run (default: journal header / config)\n"
               "  --out <file.fmj>      keep the re-simulated session journal (market-data input)\n"
               "  --expect <sha256>     expected outbound hash (default: <journal>.sha256)\n"
               "  --verify              fail (exit 1) unless every hash and message matches\n"
               "  --version | --help\n");
}

void print_replay(const fastmm::bt::ReplayResult& r) {
  std::printf("replay   strategy=%s events=%llu\n",
              r.strategy.c_str(),
              static_cast<unsigned long long>(r.events));
  std::printf("recorded outbound %llu msgs sha256 %s\n",
              static_cast<unsigned long long>(r.recorded_messages),
              r.recorded_sha256.c_str());
  std::printf("replayed outbound %llu msgs sha256 %s\n",
              static_cast<unsigned long long>(r.outbound_messages),
              r.outbound_sha256.c_str());
  if (r.first_mismatch >= 0) {
    std::printf("first mismatching outbound message: #%lld\n",
                static_cast<long long>(r.first_mismatch));
  }
  std::printf("replay %s\n", r.ok() ? "MATCH" : "MISMATCH");
}

std::string read_expected(const std::string& path) {
  std::ifstream in(path);
  std::string s;
  if (!(in >> s) || s.size() != 64) return {};
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace fastmm;
  std::string journal;
  std::string config_path;
  std::string strategy;
  std::string out;
  std::string expect;
  bool verify = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto value = [&](std::string& dst) -> bool {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "fastmm-replay: %s needs a value\n", argv[i]);
        return false;
      }
      dst = argv[++i];
      return true;
    };
    bool ok = true;
    if (a == "--help" || a == "-h") {
      usage(stdout);
      return 0;
    } else if (a == "--version") {
      std::printf("fastmm-replay %s\n", build_info());
      return 0;
    } else if (a == "--journal") {
      ok = value(journal);
    } else if (a == "--config") {
      ok = value(config_path);
    } else if (a == "--strategy") {
      ok = value(strategy);
    } else if (a == "--out") {
      ok = value(out);
    } else if (a == "--expect") {
      ok = value(expect);
    } else if (a == "--verify") {
      verify = true;
    } else {
      std::fprintf(stderr, "fastmm-replay: unknown argument '%s'\n", argv[i]);
      ok = false;
    }
    if (!ok) {
      usage(stderr);
      return kExitUsage;
    }
  }
  if (journal.empty()) {
    std::fprintf(stderr, "fastmm-replay: --journal is required\n");
    usage(stderr);
    return kExitUsage;
  }
  if (config_path.empty() && std::filesystem::exists("configs/backtest-example.toml"))
    config_path = "configs/backtest-example.toml";
  if (config_path.empty()) {
    std::fprintf(stderr,
                 "fastmm-replay: --config is required (no configs/backtest-example.toml here)\n");
    return kExitUsage;
  }

  bt::register_builtin_strategies();
  bt::BacktestConfig cfg;
  bt::JournalInfo info;
  try {
    const Config raw = Config::load(config_path);
    Logger::instance().set_level(raw.log_level());
    cfg = bt::BacktestConfig::from_config(raw);
    cfg.measure_wall_clock = false;
    cfg.output_dir.clear();
    info = bt::inspect_journal(journal);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fastmm-replay: %s\n", e.what());
    return kExitInput;
  }
  std::printf(
      "journal  %s: %llu messages (%llu market data, %llu outbound), seed %llu, strategy '%s'\n",
      journal.c_str(),
      static_cast<unsigned long long>(info.messages),
      static_cast<unsigned long long>(info.market_data_messages),
      static_cast<unsigned long long>(info.outbound_messages),
      static_cast<unsigned long long>(info.rng_seed),
      info.strategy.c_str());

  int rc = 0;
  Logger::instance().start(stderr, LogLevel::Warn);
  try {
    if (info.outbound_messages > 0) {
      bt::ReplayOptions opt;
      opt.strategy = strategy;
      opt.verify = true;
      if (!strategy.empty() && strategy != info.strategy && strategy != cfg.strategy)
        cfg.params.clear();
      const bt::ReplayResult r = bt::replay_journal(journal, cfg, opt);
      print_replay(r);
      if (verify && !r.ok()) rc = kExitMismatch;
    } else {
      // Market-data journal: backtest it, record the session, replay the session.
      if (!strategy.empty() && strategy != cfg.strategy) {
        cfg.params.clear();
        cfg.strategy = strategy;
      }
      const bool temp = out.empty();
      cfg.journal_out = temp ? (std::filesystem::temp_directory_path() /
                                ("fastmm-replay-" + std::to_string(::getpid()) + ".fmj"))
                                   .string()
                             : out;
      bt::JournalSource source(journal);
      const bt::BacktestResult sim = bt::run_backtest(cfg, cfg.strategy, &source);
      std::printf("backtest strategy=%s md_events=%llu fills=%llu outbound %llu msgs sha256 %s\n",
                  sim.strategy.c_str(),
                  static_cast<unsigned long long>(sim.md_events),
                  static_cast<unsigned long long>(sim.metrics.fills),
                  static_cast<unsigned long long>(sim.outbound_messages),
                  sim.outbound_sha256.c_str());
      bt::ReplayOptions opt;
      opt.strategy = cfg.strategy;
      const bt::ReplayResult r = bt::replay_journal(cfg.journal_out, cfg, opt);
      print_replay(r);
      bool ok = r.ok() && r.outbound_sha256 == sim.outbound_sha256;
      if (expect.empty()) {
        std::filesystem::path side(journal);
        side.replace_extension(".sha256");
        if (std::filesystem::exists(side)) {
          expect = read_expected(side.string());
          if (expect.empty())
            std::fprintf(stderr, "fastmm-replay: %s is not a sha256\n", side.c_str());
        }
      }
      if (!expect.empty()) {
        const bool golden = expect == sim.outbound_sha256;
        std::printf(
            "expected outbound sha256 %s -> %s\n", expect.c_str(), golden ? "MATCH" : "MISMATCH");
        ok = ok && golden;
      } else if (verify) {
        std::printf("no expected hash (--expect or %s.sha256); verified replay only\n",
                    std::filesystem::path(journal).replace_extension().c_str());
      }
      if (temp) {
        std::error_code ec;
        std::filesystem::remove(cfg.journal_out, ec);
      } else {
        std::printf("session journal: %s\n", cfg.journal_out.c_str());
      }
      if (verify && !ok) rc = kExitMismatch;
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fastmm-replay: %s\n", e.what());
    rc = kExitInput;
  }
  Logger::instance().stop();
  return rc;
}
