#include "fastmm/core/log.hpp"

#include <fmt/args.h>
#include <fmt/format.h>

#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace fastmm {

struct Logger::Impl {
  Impl() { rings.reserve(kLogMaxThreads); }
  std::mutex mu;                                // protects ring registration only
  std::vector<std::unique_ptr<LogRing>> rings;  // index == registration order, never shrinks
  std::atomic<std::size_t> ring_count{0};
  // in_use[i] is true while a live thread owns rings[i]; an idle ring is reusable once drained.
  std::array<std::atomic<bool>, kLogMaxThreads> in_use{};
  std::FILE* out = stderr;
  LogLevel mirror_level = LogLevel::Warn;
  std::thread sink;
  std::atomic<bool> stop_requested{false};
  std::atomic<std::uint64_t> flush_req{0};
  std::atomic<std::uint64_t> flush_done{0};
  std::string scratch;
};

Logger& Logger::instance() noexcept {
  // The first calls can come from several threads at once (sweep workers warm up their engines
  // concurrently). Function-local statics are initialised exactly once and concurrent callers
  // wait for it, so impl_ is wired before anyone gets past `wired`. It used to be a plain
  // null check, a data race. Construction order also fixes destruction order: the logger (whose
  // destructor may stop the sink) is destroyed before the Impl it uses.
  static Impl impl;
  static Logger logger;
  static const bool wired = (logger.impl_ = &impl, true);
  static_cast<void>(wired);
  return logger;
}

Logger::~Logger() {
  if (running()) stop();
}

namespace {
// Destroyed when a thread that logged exits; hands its ring back to the pool.
struct ThreadSlotRelease {
  bool armed = false;
  ThreadSlotRelease() = default;
  ThreadSlotRelease(const ThreadSlotRelease&) = delete;
  ThreadSlotRelease& operator=(const ThreadSlotRelease&) = delete;
  ~ThreadSlotRelease() {
    if (armed) Logger::instance().detach_current_thread();
    detail::t_log_thread_exiting = true;  // later thread_local destructors must not re-attach
  }
};
thread_local ThreadSlotRelease t_slot_release;
}  // namespace

LogRing* Logger::attach_current_thread() {
  if (detail::t_log_ring != nullptr) return detail::t_log_ring;
  if (detail::t_log_thread_exiting) return nullptr;
  Impl& im = *impl_;
  std::lock_guard<std::mutex> lock(im.mu);
  // Reuse a ring whose owner has exited and whose records the sink has already written. The
  // previous owner cleared its ring pointer before releasing the slot, so it can no longer
  // produce: the single-producer invariant holds across successive owners.
  std::size_t slot = im.rings.size();
  for (std::size_t i = 0; i < im.rings.size(); ++i) {
    if (!im.in_use[i].load(std::memory_order_acquire) && im.rings[i]->empty_approx()) {
      slot = i;
      break;
    }
  }
  if (slot == im.rings.size()) {
    if (im.rings.size() >= kLogMaxThreads) return nullptr;
    im.rings.push_back(std::make_unique<LogRing>());
    im.ring_count.store(im.rings.size(), std::memory_order_release);
  }
  im.in_use[slot].store(true, std::memory_order_release);
  t_slot_release.armed = true;
  detail::t_log_slot = static_cast<std::uint32_t>(slot);
  detail::t_log_tid = static_cast<std::uint32_t>(::syscall(SYS_gettid));
  detail::t_log_ring = im.rings[slot].get();
  return detail::t_log_ring;
}

void Logger::detach_current_thread() noexcept {
  if (detail::t_log_ring == nullptr) return;
  detail::t_log_ring = nullptr;  // stop producing first, then publish the slot as free
  impl_->in_use[detail::t_log_slot].store(false, std::memory_order_release);
}

std::size_t Logger::thread_slots() const noexcept {
  return impl_->ring_count.load(std::memory_order_acquire);
}

namespace {

void format_iso8601(std::int64_t ns, std::string& out) {
  const time_t secs = ns / 1'000'000'000;
  const std::int64_t frac = ns % 1'000'000'000;
  tm t{};
  gmtime_r(&secs, &t);
  fmt::format_to(std::back_inserter(out),
                 "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:09}Z",
                 t.tm_year + 1900,
                 t.tm_mon + 1,
                 t.tm_mday,
                 t.tm_hour,
                 t.tm_min,
                 t.tm_sec,
                 frac);
}

std::string_view basename_of(const char* path) noexcept {
  std::string_view sv(path);
  const auto slash = sv.find_last_of('/');
  return slash == std::string_view::npos ? sv : sv.substr(slash + 1);
}

}  // namespace

void Logger::format_record(const LogRecord& r, std::string& out) {
  format_iso8601(r.ts_ns, out);
  fmt::format_to(std::back_inserter(out),
                 " {:<5} [{}] {}:{} ",
                 to_string(r.desc->level),
                 r.tid,
                 basename_of(r.desc->file),
                 r.desc->line);

  fmt::dynamic_format_arg_store<fmt::format_context> store;
  // Decimal strings for Fixed args must outlive vformat; keep them here.
  std::vector<std::string> owned;
  owned.reserve(r.nargs);
  const std::uint8_t* p = r.args;
  const std::uint8_t* end = r.args + r.used;
  for (std::uint8_t i = 0; i < r.nargs && p < end; ++i) {
    const auto type = static_cast<LogArgType>(*p++);
    if (type == LogArgType::Str || type == LogArgType::Secret) {
      const std::size_t len = *p++;
      store.push_back(std::string_view(reinterpret_cast<const char*>(p), len));
      p += len;
      continue;
    }
    std::uint64_t bits = 0;
    std::memcpy(&bits, p, 8);
    p += 8;
    switch (type) {
      case LogArgType::Int:
        store.push_back(static_cast<std::int64_t>(bits));
        break;
      case LogArgType::Uint:
        store.push_back(bits);
        break;
      case LogArgType::Double: {
        double d = 0;
        std::memcpy(&d, &bits, 8);
        store.push_back(d);
        break;
      }
      case LogArgType::Bool:
        store.push_back(bits != 0);
        break;
      case LogArgType::Char:
        store.push_back(static_cast<char>(bits));
        break;
      case LogArgType::PriceFx:
      case LogArgType::QtyFx:
      case LogArgType::NotionalFx: {
        char buf[kMaxDecimalChars];
        const std::size_t n = Price::from_raw(static_cast<std::int64_t>(bits)).to_decimal(buf);
        owned.emplace_back(buf, n);
        store.push_back(std::string_view(owned.back()));
        break;
      }
      case LogArgType::Str:
      case LogArgType::Secret:
        break;  // handled above
    }
  }
  try {
    fmt::vformat_to(std::back_inserter(out), r.desc->fmt, store);
  } catch (const fmt::format_error& e) {
    fmt::format_to(std::back_inserter(out), "<format error: {}> fmt=\"{}\"", e.what(), r.desc->fmt);
  }
  out.push_back('\n');
}

std::size_t Logger::drain_all() {
  Impl& im = *impl_;
  std::size_t processed = 0;
  const std::size_t n = im.ring_count.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < n; ++i) {
    LogRing& ring = *im.rings[i];
    while (const LogRecord* rec = ring.front()) {
      im.scratch.clear();
      format_record(*rec, im.scratch);
      const LogLevel lvl = rec->desc->level;
      ring.pop();
      std::fwrite(im.scratch.data(), 1, im.scratch.size(), im.out);
      if (im.out != stderr && lvl >= im.mirror_level) {
        std::fwrite(im.scratch.data(), 1, im.scratch.size(), stderr);
      }
      ++processed;
      written_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return processed;
}

void Logger::sink_loop() {
  Impl& im = *impl_;
  int idle = 0;
  while (!im.stop_requested.load(std::memory_order_acquire)) {
    // Read the flush request *before* draining so every record published before the
    // request is guaranteed to be on disk when flush_done catches up.
    const std::uint64_t req = im.flush_req.load(std::memory_order_acquire);
    const std::size_t n = drain_all();
    if (req != im.flush_done.load(std::memory_order_relaxed)) {
      std::fflush(im.out);
      im.flush_done.store(req, std::memory_order_release);
    }
    if (n == 0) {
      if (++idle > 100) {
        timespec ts{0, 50'000};
        nanosleep(&ts, nullptr);
      } else {
        __builtin_ia32_pause();
      }
    } else {
      idle = 0;
    }
  }
  drain_all();
  std::fflush(im.out);
}

void Logger::start(std::FILE* out, LogLevel mirror_level) {
  if (running()) return;
  Impl& im = *impl_;
  im.out = out == nullptr ? stderr : out;
  im.mirror_level = mirror_level;
  im.stop_requested.store(false);
  running_.store(true, std::memory_order_release);
  im.sink = std::thread([this] { sink_loop(); });
}

void Logger::stop() {
  if (!running()) return;
  Impl& im = *impl_;
  im.stop_requested.store(true, std::memory_order_release);
  im.sink.join();
  running_.store(false, std::memory_order_release);
}

void Logger::flush() {
  Impl& im = *impl_;
  if (!running()) {
    drain_all();
    std::fflush(im.out);
    return;
  }
  const std::uint64_t req = im.flush_req.fetch_add(1, std::memory_order_acq_rel) + 1;
  while (im.flush_done.load(std::memory_order_acquire) < req) {
    timespec ts{0, 20'000};
    nanosleep(&ts, nullptr);
  }
}

}  // namespace fastmm
