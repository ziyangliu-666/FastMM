#include "fastmm/core/journal_archive.hpp"

#include "fastmm/core/log.hpp"
#include "fastmm/core/thread_utils.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <zstd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fastmm {

namespace {

constexpr std::string_view kJournalExt = ".fmj";
constexpr std::string_view kArchiveExt = ".zst";
constexpr std::string_view kTmpExt = ".tmp";
constexpr std::size_t kChunk = std::size_t{1} << 20;

bool ends_with(std::string_view s, std::string_view tail) noexcept {
  return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail;
}

// A file descriptor closed on scope exit.
struct Fd {
  int fd = -1;
  Fd() = default;
  explicit Fd(int f) noexcept : fd(f) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  ~Fd() {
    if (fd >= 0) ::close(fd);
  }
};

void note(std::string* error, std::string what) {
  if (error != nullptr && error->empty()) *error = std::move(what);
}

// A journal file of the directory, either form.
struct Entry {
  std::string path;
  std::uint64_t bytes = 0;
  std::int64_t mtime_ns = 0;
  bool archive = false;  // x.fmj.zst
};

std::int64_t mtime_ns(const struct stat& st) noexcept {
  return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000LL + st.st_mtim.tv_nsec;
}

// Open for reading with a shared lock taken, or -1 when its writer holds the lock (or it cannot
// be opened). The lock goes with the descriptor.
int open_unlocked(const std::string& path) noexcept {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  if (::flock(fd, LOCK_SH | LOCK_NB) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool in_use(const Entry& e, std::int64_t now_ns, std::int64_t idle_s) noexcept {
  if (now_ns - e.mtime_ns < idle_s * 1'000'000'000LL) return true;
  const Fd fd(open_unlocked(e.path));
  return fd.fd < 0;
}

bool write_all(int fd, const void* data, std::size_t len) noexcept {
  const auto* p = static_cast<const char*>(data);
  while (len > 0) {
    const ssize_t n = ::write(fd, p, len);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    p += n;
    len -= static_cast<std::size_t>(n);
  }
  return true;
}

std::int64_t wall_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

std::string journal_base_path(const std::string& path) {
  if (ends_with(path, std::string(kJournalExt) + std::string(kArchiveExt)))
    return path.substr(0, path.size() - kArchiveExt.size());
  return path;
}

bool compress_journal(const std::string& from,
                      const std::string& to,
                      int level,
                      const std::atomic<bool>* stop,
                      std::string* error) {
  const Fd in(::open(from.c_str(), O_RDONLY | O_CLOEXEC));
  if (in.fd < 0) {
    note(error, "cannot open " + from + ": " + std::strerror(errno));
    return false;
  }
  struct stat st {};
  if (::fstat(in.fd, &st) != 0) {
    note(error, "cannot stat " + from + ": " + std::strerror(errno));
    return false;
  }
  const std::string tmp = to + std::string(kTmpExt);
  Fd out(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
  if (out.fd < 0) {
    note(error, "cannot create " + tmp + ": " + std::strerror(errno));
    return false;
  }
  const auto give_up = [&](std::string why) {
    ::close(out.fd);
    out.fd = -1;
    ::unlink(tmp.c_str());
    note(error, std::move(why));
    return false;
  };
  const std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> cctx(ZSTD_createCCtx(),
                                                                  &ZSTD_freeCCtx);
  if (cctx == nullptr) return give_up("zstd: cannot allocate a context");
  ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_compressionLevel, level);
  ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_checksumFlag, 1);
  // The size goes in the frame header, so a reader sizes its buffer once.
  ZSTD_CCtx_setPledgedSrcSize(cctx.get(), static_cast<unsigned long long>(st.st_size));
  std::vector<char> src(kChunk);
  std::vector<char> dst(ZSTD_CStreamOutSize());
  std::uint64_t read_total = 0;
  for (;;) {
    if (stop != nullptr && stop->load(std::memory_order_relaxed)) return give_up("stopped");
    ssize_t n = ::read(in.fd, src.data(), src.size());
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) return give_up("cannot read " + from + ": " + std::strerror(errno));
    read_total += static_cast<std::uint64_t>(n);
    // The pledged size ends the frame: a file that is still growing is not one to archive.
    const bool last = n == 0 || read_total >= static_cast<std::uint64_t>(st.st_size);
    if (read_total > static_cast<std::uint64_t>(st.st_size))
      return give_up(from + " grew while it was compressed");
    ZSTD_inBuffer ib{src.data(), static_cast<std::size_t>(n), 0};
    const ZSTD_EndDirective mode = last ? ZSTD_e_end : ZSTD_e_continue;
    for (;;) {
      ZSTD_outBuffer ob{dst.data(), dst.size(), 0};
      const std::size_t left = ZSTD_compressStream2(cctx.get(), &ob, &ib, mode);
      if (ZSTD_isError(left) != 0) return give_up(std::string("zstd: ") + ZSTD_getErrorName(left));
      if (!write_all(out.fd, dst.data(), ob.pos))
        return give_up("cannot write " + tmp + ": " + std::strerror(errno));
      if (last ? left == 0 : ib.pos == ib.size) break;
    }
    if (last) break;
  }
  if (::fsync(out.fd) != 0) return give_up("cannot sync " + tmp + ": " + std::strerror(errno));
  // The archive carries the journal's time: retention and the cap order by when it was written.
  const timespec times[2] = {st.st_atim, st.st_mtim};
  static_cast<void>(::futimens(out.fd, times));
  ::close(out.fd);
  out.fd = -1;
  if (::rename(tmp.c_str(), to.c_str()) != 0) {
    ::unlink(tmp.c_str());
    note(error, "cannot rename " + tmp + ": " + std::strerror(errno));
    return false;
  }
  return true;
}

JournalSweep sweep_journals(const std::string& dir,
                            const JournalRetention& r,
                            bool compress,
                            const std::atomic<bool>* stop) {
  JournalSweep out;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return out;
  const std::int64_t now = wall_ns();
  const auto list = [&] {
    std::vector<Entry> v;
    for (const auto& de : std::filesystem::directory_iterator(dir, ec)) {
      const std::string path = de.path().string();
      const bool archive = ends_with(path, std::string(kJournalExt) + std::string(kArchiveExt));
      // A compression that did not finish (the process died in it): its leftover goes.
      if (ends_with(path,
                    std::string(kJournalExt) + std::string(kArchiveExt) + std::string(kTmpExt))) {
        struct stat st {};
        if (::stat(path.c_str(), &st) == 0 && now - mtime_ns(st) > r.idle_s * 1'000'000'000LL)
          ::unlink(path.c_str());
        continue;
      }
      if (!archive && !ends_with(path, kJournalExt)) continue;
      struct stat st {};
      if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
      v.push_back(Entry{path, static_cast<std::uint64_t>(st.st_size), mtime_ns(st), archive});
    }
    // Oldest first; a session's parts by name when they share a time.
    std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) {
      return a.mtime_ns != b.mtime_ns ? a.mtime_ns < b.mtime_ns : a.path < b.path;
    });
    return v;
  };
  std::vector<Entry> files = list();

  if (compress && r.compress) {
    for (Entry& e : files) {
      if (stop != nullptr && stop->load(std::memory_order_relaxed)) return out;
      if (e.archive) continue;
      const Fd locked(open_unlocked(e.path));
      if (locked.fd < 0 || now - e.mtime_ns < r.idle_s * 1'000'000'000LL) continue;
      const std::string to = e.path + std::string(kArchiveExt);
      std::string why;
      if (!compress_journal(e.path, to, r.level, stop, &why)) {
        if (why != "stopped") note(&out.error, why);
        continue;
      }
      struct stat st {};
      const std::uint64_t to_bytes =
          ::stat(to.c_str(), &st) == 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
      if (::unlink(e.path.c_str()) != 0) {
        note(&out.error, "cannot remove " + e.path + ": " + std::strerror(errno));
        continue;
      }
      ++out.compressed;
      out.compressed_from += e.bytes;
      out.compressed_to += to_bytes;
    }
    files = list();
  }

  const auto remove = [&](const Entry& e) {
    if (in_use(e, now, r.idle_s)) return false;
    if (::unlink(e.path.c_str()) != 0) {
      note(&out.error, "cannot remove " + e.path + ": " + std::strerror(errno));
      return false;
    }
    out.removed_bytes += e.bytes;
    return true;
  };
  std::vector<bool> gone(files.size(), false);
  if (r.retention_days > 0) {
    const std::int64_t cutoff =
        now - static_cast<std::int64_t>(r.retention_days) * 86'400'000'000'000LL;
    for (std::size_t i = 0; i < files.size(); ++i) {
      if (files[i].mtime_ns >= cutoff) continue;
      if (remove(files[i])) {
        gone[i] = true;
        ++out.removed_old;
      }
    }
  }
  std::uint64_t total = 0;
  for (std::size_t i = 0; i < files.size(); ++i) {
    if (!gone[i]) total += files[i].bytes;
  }
  if (r.max_total_bytes > 0) {
    for (std::size_t i = 0; i < files.size() && total > r.max_total_bytes; ++i) {
      if (gone[i] || !remove(files[i])) continue;
      gone[i] = true;
      total -= files[i].bytes;
      ++out.removed_cap;
    }
    out.over_cap = total > r.max_total_bytes;
  }
  out.total_bytes = total;
  return out;
}

// ---- JournalArchiver ------------------------------------------------------------------------

JournalArchiver::JournalArchiver(std::string dir, JournalRetention r, Duration period)
    : dir_(std::move(dir)), r_(r), period_(period) {}

JournalArchiver::~JournalArchiver() {
  stop();
}

void JournalArchiver::start() {
  if (thread_.joinable()) return;
  stop_.store(false, std::memory_order_relaxed);
  thread_ = std::thread([this] { run(); });
}

void JournalArchiver::stop() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stop_.store(true, std::memory_order_relaxed);
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void JournalArchiver::run() {
  set_thread_name("fm-archive");
  Logger::instance().attach_current_thread();
  // Compression is the one CPU-heavy thing a session does off the engine: below everything else.
  static_cast<void>(::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 19));
  while (!stop_.load(std::memory_order_relaxed)) {
    const JournalSweep s = sweep_journals(dir_, r_, true, &stop_);
    sweeps_.fetch_add(1, std::memory_order_relaxed);
    compressed_.fetch_add(s.compressed, std::memory_order_relaxed);
    removed_.fetch_add(s.removed_old + s.removed_cap, std::memory_order_relaxed);
    if (s.compressed != 0) {
      FASTMM_LOG_INFO("journal archive: {} journal(s) compressed in {}, {} -> {} bytes",
                      s.compressed,
                      dir_,
                      s.compressed_from,
                      s.compressed_to);
    }
    if (s.removed_old + s.removed_cap != 0) {
      FASTMM_LOG_INFO(
          "journal archive: {} journal(s) removed from {} ({} older than {} day(s), {} over the "
          "{}-byte cap), {} bytes; {} bytes left",
          s.removed_old + s.removed_cap,
          dir_,
          s.removed_old,
          r_.retention_days,
          s.removed_cap,
          r_.max_total_bytes,
          s.removed_bytes,
          s.total_bytes);
    }
    if (s.over_cap && !over_cap_logged_) {
      FASTMM_LOG_WARN(
          "journal archive: the journals in {} total {} bytes, over journal_max_total_bytes {}, "
          "and what is left is in use",
          dir_,
          s.total_bytes,
          r_.max_total_bytes);
    }
    over_cap_logged_ = s.over_cap;
    if (!s.error.empty()) FASTMM_LOG_WARN("journal archive: {}", std::string_view(s.error));
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait_for(lock, std::chrono::nanoseconds(period_.ns), [this] {
      return stop_.load(std::memory_order_relaxed);
    });
  }
}

}  // namespace fastmm
