#pragma once
// Shadow sweeps: the variants of a strategy's parameters run over a live session's journal while
// the session writes it, each by the rules of a backtest of that journal, and the first point, at
// the session's own parameters, set beside the session's real fills.
//
//   session ---journal---> FollowSource ---> a multiverse's passes ---> windows ---> report, status
//
// A process of its own reads the file the session already writes; the session does not know.
// Everything the report says is a function of the journal: a window's numbers do not depend on how
// far the journal had been written when they were computed, so a shadow that restarts recomputes
// them (tests/backtest/shadow_exact_test.cpp).
//
// The runs start where the session started: its restored state, the positions it reconciled before
// its first order and its first balance snapshots. They follow its parameter updates, except those
// of the parameters a variant sets, its operator commands and the flows of money in and out of its
// accounts. A session's restart is a new journal: a new run of every point from where that session
// started, its windows added to the report's.
//
// Windows cut the session into `window` of venue time. For each the report has the net PnL (the
// ledger of a backtest, starting inventory aside, marked at the same mids), the fills, the notional
// and the orders of the session and of every point, and per instrument those of the session and the
// live point. Two comparisons follow: the simulation against live, per window and in total, and
// each variant against the live point, in pairs per window. The intervals resample runs of windows
// jointly across the variants, so they hold for all the variants at once: the best of a thousand is
// not better by luck of the draw.
#include "fastmm/backtest/backtest_config.hpp"
#include "fastmm/backtest/follow_source.hpp"
#include "fastmm/backtest/multiverse.hpp"
#include "fastmm/backtest/result.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fastmm {
struct ShadowSection;
class Config;
}  // namespace fastmm

namespace fastmm::bt {

struct ShadowOptions {
  // The variants; the live point, which sets nothing, always runs first as point 0.
  SweepPoints variants;
  int threads = 1;
  Duration window = seconds(60);
  Duration settle = seconds(10);
  Duration max_lag{seconds(300)};  // 0: no variant ever stops
  std::string report_path;         // empty: no report (and no <stem>.jsonl beside it)
  std::string status_path;         // empty: no status file
  const std::atomic<bool>* stop = nullptr;
  // Keep every run's rows, for a result that is the whole of what a backtest returns. Off, a run
  // holds its current window and memory stays flat over a session of any length.
  bool keep_results = false;
  // Builds a run; empty: the journal's strategy from the registry. (The Python package's hot
  // strategies come this way.)
  MultiverseFactory factory;
  // The configuration of the runs in place of the one the journal embeds (a journal of a build
  // with keys this one does not know). Empty: the embedded one.
  std::string config_text;
  // Changes the configuration of the session's runs before the points are applied to it.
  std::function<void(BacktestConfig&)> configure;
  // The timings of the journal's reader; `settle`, `stop` and `on_record` are set from here.
  FollowOptions follow;
};

// What the configuration of a run over this session is: the embedded configuration, with the
// journal's own instrument table and session settings, the session's start and updates to follow,
// and bars of `window`. A backtest of the finished journal with this configuration and
// JournalSource(path, strip_own = the journal is live's) is what a shadow point equals.
// `config_text`: this configuration in place of the embedded one.
[[nodiscard]] BacktestConfig shadow_config(const JournalReader& prologue,
                                           const sim::RecordedSession& recorded,
                                           Duration window,
                                           std::string_view config_text = {});

// A simultaneous interval on the sums of paired series.
struct PairedSum {
  double sum = 0.0;
  double lo = 0.0;  // NaN below kMinWindowsForInterval windows
  double hi = 0.0;
};
inline constexpr std::size_t kMinWindowsForInterval = 8;
// series[v][w]: the sum over w for each v, with a moving-block bootstrap of the windows (blocks of
// about n^(1/3)), max-t over the series for a family-wise `level`.
[[nodiscard]] std::vector<PairedSum> simultaneous_sums(
    const std::vector<std::vector<double>>& series,
    double level = 0.95,
    std::size_t resamples = 1000,
    std::uint64_t seed = 1);

struct ShadowSessionResult {
  std::uint64_t session_id = 0;
  std::string path;
  bool empty = false;    // no market data in it
  bool stopped = false;  // the caller's stop cut it short
  std::uint64_t md_events = 0;
  std::int64_t first_ns = 0;  // venue times of the first and the last market data
  std::int64_t last_ns = 0;
  std::uint64_t parts = 0;
  std::uint64_t late = 0;        // FollowSource::late
  std::uint64_t late_start = 0;  // FollowSource::late_start
  std::uint64_t seq_gaps = 0;
  bool writer_gone = false;
  std::vector<BacktestResult> results;  // by point; the rows only with keep_results
};

// One shadow: a report that goes on across the sessions it is given.
class Shadow {
 public:
  explicit Shadow(ShadowOptions options);
  ~Shadow();
  Shadow(const Shadow&) = delete;
  Shadow& operator=(const Shadow&) = delete;

  // Follows one session from its first record until it ends (or stop). Throws what a backtest of
  // it would throw (a strategy this build does not have, a configuration that cannot share a pass).
  ShadowSessionResult session(const std::string& journal_path);
  // <dir>/<name>-<id>.fmj, the newest, then each one after it, until stop, or (idle_exit above 0)
  // until no session has come for that long after the last ended.
  void follow(const std::string& dir, const std::string& name, Duration idle_exit = {});
  // The report as of now; empty before the first window.
  [[nodiscard]] std::string report_json() const;
  // The windows closed so far, over all the sessions.
  [[nodiscard]] std::size_t windows() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The variants [shadow] grid and points name, and the settings of its other keys.
[[nodiscard]] ShadowOptions shadow_options(const ShadowSection& section);

}  // namespace fastmm::bt
