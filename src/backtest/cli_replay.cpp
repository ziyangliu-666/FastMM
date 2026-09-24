// fastmm::cli::replay: deterministic replay proof (5.11). The fastmm-replay app is
// `return fastmm::cli::replay(argc, argv);`; a journal recorded by a custom app replays with an app
// that registers the same strategy modules.
//
//   fastmm-replay --journal session.fmj [--config cfg.toml] [--strategy name] [--verify]
//   fastmm-replay --journal tests/fixtures/journals/sample_1000.fmj --verify
//
// Session journal (recorded with outbound copies: fastmm-live, fastmm-backtest --journal-out):
//   Engine<S, SimClock, ReplayTransport, JournalFeed> replays the inbound events at the recorded
//   engine clock; the outbound SHA-256 and every message are compared with the recorded Out*
//   records, and the first differing message is printed. The configuration embedded in the
//   journal is used unless --config is given; --config is checked against the journal's config
//   hash (a different configuration is a what-if run and is reported as such). Session epoch,
//   dry run, RNG seed and per-venue cancel-replace always come from the journal. A journal without
//   an embedded configuration (format v1) needs --config. No API keys or ${VAR}s are needed.
// Market-data journal (no outbound records, e.g. the golden fixture):
//   the strategy is backtested on the journal (SimTransport), the run is recorded to
//   --out (a temporary file by default) and that session is replayed as above. With --verify
//   the backtest's outbound hash must also equal the sidecar <journal>.sha256 (or
//   --expect <hex>).
//
// Exit codes: 0 match (or no verification requested), 1 mismatch, 2 bad command line,
// 3 unreadable config / journal / unknown strategy (including a strategy name registered twice by
// different code).
#include "command_line.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/cli/modules.hpp"
#include "fastmm/cli/replay.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/core/log.hpp"

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace fastmm::cli {

namespace {

constexpr int kExitMismatch = 1;
constexpr int kExitUsage = kUsageError;
constexpr int kExitInput = 3;

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
    std::printf("  expected: %s\n", r.expected_message.c_str());
    std::printf("  actual:   %s\n", r.actual_message.c_str());
  }
  std::printf("replay %s\n", r.ok() ? "MATCH" : "MISMATCH");
}

fastmm::Config load_config(const std::string& path) {
  fastmm::Config::LoadOptions lo;
  lo.substitute_env = false;  // replay sends nothing: API keys and ${VAR}s stay unresolved
  return fastmm::Config::load(path, lo);
}

void print_config_warnings(const char* prog,
                           const std::string& path,
                           const fastmm::bt::BacktestConfig& cfg) {
  for (const std::string& w : cfg.warnings)
    std::fprintf(stderr, "%s: warning: %s: %s\n", prog, path.c_str(), w.c_str());
}

std::string read_expected(const std::string& path) {
  std::ifstream in(path);
  std::string s;
  if (!(in >> s) || s.size() != 64) return {};
  return s;
}

}  // namespace

int replay(int argc, char** argv, std::span<const StrategyModule> modules) {
  const std::string program = program_name(argc, argv, "fastmm-replay");
  const char* prog = program.c_str();
  std::string journal;
  std::string config_path;
  std::string strategy;
  std::string out;
  std::string expect;
  bool verify = false;
  bool allow_incomplete = false;
  CLI::App app("Replays a journal through the same engine and strategy.", program);
  setup(app);
  app.add_option("--journal", journal, "session or market-data journal to replay (required)")
      ->option_text("<in.fmj>");
  app.add_option("--config",
                 config_path,
                 "configuration to replay with (default: the one embedded in a session journal; "
                 "configs/backtest-example.toml for a market-data journal)")
      ->option_text("<file.toml>");
  app.add_option("--strategy", strategy, "strategy to run (default: journal header / config)")
      ->option_text("<name>");
  app.add_option("--out", out, "keep the re-simulated session journal (market-data input)")
      ->option_text("<file.fmj>");
  app.add_option("--expect", expect, "expected outbound hash (default: <journal>.sha256)")
      ->option_text("<sha256>");
  app.add_flag("--verify", verify, "fail (exit 1) unless every hash and message matches");
  app.add_flag("--allow-incomplete",
               allow_incomplete,
               "replay a journal the writer never closed; its tail is missing, so the outbound "
               "comparison proves nothing");
  if (const std::optional<int> rc = parse(app, argc, argv)) return *rc;
  // Checked here rather than with required(): an unknown flag is the better message.
  if (journal.empty()) return usage_error(app, "--journal is required");
  if (!register_strategy_modules(program, modules)) return kExitInput;
  bt::JournalInfo info;
  try {
    info = bt::inspect_journal(journal);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", prog, e.what());
    return kExitInput;
  }
  if (!info.complete) {
    std::fprintf(stderr,
                 "%s: %s was not closed by its writer: its last block is missing or damaged.%s\n",
                 prog,
                 journal.c_str(),
                 allow_incomplete ? " Replaying what is there." : "");
    if (!allow_incomplete) {
      std::fprintf(
          stderr, "%s: pass --allow-incomplete to replay the events before that point\n", prog);
      return kExitInput;
    }
  }
  const bool session = info.outbound_messages > 0;
  std::printf(
      "journal  %s: format v%u, %llu messages (%llu market data, %llu outbound), rng_seed %llu, "
      "strategy '%s'\n",
      journal.c_str(),
      info.version,
      static_cast<unsigned long long>(info.messages),
      static_cast<unsigned long long>(info.market_data_messages),
      static_cast<unsigned long long>(info.outbound_messages),
      static_cast<unsigned long long>(info.rng_seed),
      info.strategy.c_str());
  if (info.has_session) {
    std::printf("session  epoch %u, quoting %s, cancel-replace venues %#llx, engine clock %s\n",
                static_cast<unsigned>(info.session_epoch),
                info.quoting_enabled ? "enabled" : "disabled (dry run)",
                static_cast<unsigned long long>(info.replace_venues),
                info.engine_time ? "recorded" : "not recorded");
  }
  if (info.dropped_outbound > 0) {
    std::printf("         %llu outbound message(s) were refused by the transport\n",
                static_cast<unsigned long long>(info.dropped_outbound));
  }

  bt::BacktestConfig cfg;
  try {
    if (!config_path.empty()) {
      const Config raw = load_config(config_path);
      Logger::instance().set_level(raw.log_level());
      cfg = bt::BacktestConfig::from_config(raw);
      print_config_warnings(prog, config_path, cfg);
      if (session && !info.config_toml.empty()) {
        const std::uint64_t h = raw.effective_hash();
        if (h == info.config_hash) {
          std::printf("config   %s (matches the recording, hash %016llx)\n",
                      config_path.c_str(),
                      static_cast<unsigned long long>(h));
        } else {
          std::fprintf(stderr,
                       "%s: warning: %s is not the configuration this session was "
                       "recorded with (effective config hash %016llx, journal %016llx); this is a "
                       "what-if replay and is not expected to match. Omit --config to replay "
                       "with the recorded configuration.\n",
                       prog,
                       config_path.c_str(),
                       static_cast<unsigned long long>(h),
                       static_cast<unsigned long long>(info.config_hash));
          std::printf("config   %s (differs from the recording)\n", config_path.c_str());
        }
      } else if (session) {
        std::printf("config   %s (not checked: the journal embeds no configuration)\n",
                    config_path.c_str());
      }
    } else if (session) {
      if (info.config_toml.empty()) {
        std::fprintf(stderr,
                     "%s: %s does not embed its configuration (journal format v%u); "
                     "pass --config <the configuration the session ran with>\n",
                     prog,
                     journal.c_str(),
                     info.version);
        return kExitUsage;
      }
      cfg = bt::journal_config(journal);
      std::printf("config   embedded in the journal (hash %016llx)\n",
                  static_cast<unsigned long long>(info.config_hash));
    } else {
      // Market-data journal: backtested with a configuration first.
      config_path = "configs/backtest-example.toml";
      if (!std::filesystem::exists(config_path)) {
        std::fprintf(stderr,
                     "%s: --config is required for a market-data journal (no "
                     "configs/backtest-example.toml here)\n",
                     prog);
        return kExitUsage;
      }
      std::printf("config   %s (default for a market-data journal)\n", config_path.c_str());
      const Config raw = load_config(config_path);
      Logger::instance().set_level(raw.log_level());
      cfg = bt::BacktestConfig::from_config(raw);
      print_config_warnings(prog, config_path, cfg);
    }
    cfg.measure_wall_clock = false;
    cfg.output_dir.clear();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", prog, e.what());
    return kExitInput;
  }

  int rc = 0;
  Logger::instance().start(stderr, LogLevel::Warn);
  try {
    if (session) {
      bt::ReplayOptions opt;
      opt.strategy = strategy;
      opt.verify = true;
      opt.allow_incomplete = allow_incomplete;
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
          if (expect.empty()) std::fprintf(stderr, "%s: %s is not a sha256\n", prog, side.c_str());
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
    std::fprintf(stderr, "%s: %s\n", prog, e.what());
    rc = kExitInput;
  }
  Logger::instance().stop();
  return rc;
}

}  // namespace fastmm::cli
