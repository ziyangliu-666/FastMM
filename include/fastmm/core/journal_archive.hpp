#pragma once
// Journal archive: what [engine] journal_compress, journal_retention_days and
// journal_max_total_bytes do to the journals in journal_dir.
//
// A sweep of the directory, in this order:
//   1. compresses each .fmj nobody is writing into x.fmj.zst (zstd), keeping the file's time, and
//      deletes the original once the archive is on disk. JournalReader opens either form, and
//      asking for x.fmj finds x.fmj.zst, so a replay, a backtest or a tool is given the same path
//      as before;
//   2. deletes the journals, either form, last written more than retention_days ago;
//   3. while the journals total more than max_total_bytes, deletes the oldest.
// A journal is in use, and a sweep leaves it alone, while its writer holds its lock
// (JournalFileWriter locks each part it writes, so another engine sharing the directory is safe
// too) or while it was written to less than idle_s ago (a writer of an older build). Parts in use
// count towards the cap but are never deleted, so a cap they exceed alone stays exceeded
// (JournalSweep::over_cap).
//
// fastmm-live sweeps once at start without compressing (the new journal finds the room it needs)
// and then runs a JournalArchiver: a thread of its own, at the lowest CPU priority, on the cores of
// the other threads, sweeping every minute. Compression streams 1 MiB at a time, so it holds a few
// MiB whatever the size of the part, and stops between chunks when asked to; an archive it did not
// finish is deleted, never left behind as x.fmj.zst.
#include "fastmm/core/time.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace fastmm {

struct JournalRetention {
  bool compress = false;              // [engine] journal_compress = "zstd"
  int level = 3;                      // zstd level
  int retention_days = 0;             // [engine] journal_retention_days; 0: keep
  std::uint64_t max_total_bytes = 0;  // [engine] journal_max_total_bytes; 0: no cap
  std::int64_t idle_s = 60;           // a journal written to more recently is in use
  [[nodiscard]] bool any() const noexcept {
    return compress || retention_days > 0 || max_total_bytes > 0;
  }
};

struct JournalSweep {
  std::size_t compressed = 0;
  std::uint64_t compressed_from = 0;  // bytes before and after, of the journals compressed
  std::uint64_t compressed_to = 0;
  std::size_t removed_old = 0;  // older than retention_days
  std::size_t removed_cap = 0;  // over max_total_bytes
  std::uint64_t removed_bytes = 0;
  std::uint64_t total_bytes = 0;  // the journals left, in use included
  bool over_cap = false;          // still over max_total_bytes: what is left is in use
  std::string error;              // the first thing that failed; the sweep went on past it
};

// One sweep of `dir` (nothing when it does not exist). `compress` false skips step 1 whatever
// `r.compress` says. `stop`, when set, ends a compression between chunks.
[[nodiscard]] JournalSweep sweep_journals(const std::string& dir,
                                          const JournalRetention& r,
                                          bool compress = true,
                                          const std::atomic<bool>* stop = nullptr);

// Compresses the journal `from` into `to` (zstd at `level`), through `to`.tmp renamed when
// complete and synced; `from` is left alone. False with `error` set when it failed or `stop` ended
// it, and no `to` then.
bool compress_journal(const std::string& from,
                      const std::string& to,
                      int level,
                      const std::atomic<bool>* stop = nullptr,
                      std::string* error = nullptr);

// "x.fmj" for "x.fmj" and for "x.fmj.zst".
[[nodiscard]] std::string journal_base_path(const std::string& path);

// Runs sweep_journals on its own thread every `period` until stop(). The first sweep starts at
// once.
class JournalArchiver {
 public:
  JournalArchiver(std::string dir, JournalRetention r, Duration period = seconds(60));
  ~JournalArchiver();
  JournalArchiver(const JournalArchiver&) = delete;
  JournalArchiver& operator=(const JournalArchiver&) = delete;

  void start();
  void stop();  // ends a compression in progress; joins

  [[nodiscard]] std::uint64_t sweeps() const noexcept {
    return sweeps_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t compressed() const noexcept {
    return compressed_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t removed() const noexcept {
    return removed_.load(std::memory_order_relaxed);
  }

 private:
  void run();

  std::string dir_;
  JournalRetention r_;
  Duration period_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::condition_variable wake_;
  std::atomic<std::uint64_t> sweeps_{0};
  std::atomic<std::uint64_t> compressed_{0};
  std::atomic<std::uint64_t> removed_{0};
  bool over_cap_logged_ = false;
};

}  // namespace fastmm
