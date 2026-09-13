#pragma once
// Asynchronous logging (5.10).
//
//   FASTMM_LOG_INFO("filled {} @ {} on {}", qty, px, side);
//
// The call site captures a pointer to a static LogDescriptor (format string, file, line,
// level) plus the packed argument values into a 256-byte LogRecord in the calling thread's
// SpscRing. Nothing is formatted on the caller's thread; the LogSink thread pops records,
// formats them with fmt and writes them to a FILE*. When a ring is full the record is
// dropped and counted - the hot path never blocks.
//
// The format string is validated at compile time against the argument types (fmt), so a
// mismatch is a compile error rather than a runtime surprise on the sink thread.
//
// Supported argument types: integers, floating point, bool, char, Price/Qty/Notional
// (formatted as exact decimals), enums (via to_string() when available, else the
// underlying integer), string_view / const char* / std::string / FixedString (copied,
// truncated to kLogMaxStrBytes), Timestamp/Duration (ns as integers), Secret<T> ("***").
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/fixed_string.hpp"
#include "fastmm/core/spsc_ring.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <fmt/base.h>

#include <atomic>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

namespace fastmm {

struct LogDescriptor {
  const char* fmt;
  const char* file;
  std::uint32_t line;
  LogLevel level;
};

enum class LogArgType : std::uint8_t {
  Int = 1,
  Uint = 2,
  Double = 3,
  Bool = 4,
  Char = 5,
  PriceFx = 6,
  QtyFx = 7,
  NotionalFx = 8,
  Str = 9,
  Secret = 10,
};

inline constexpr std::size_t kLogRecordBytes = 256;
inline constexpr std::size_t kLogMaxStrBytes = 64;
inline constexpr std::size_t kLogRingSize = 8192;  // records per thread (2 MiB)
inline constexpr std::size_t kLogMaxThreads = 64;

struct LogRecord {
  const LogDescriptor* desc;
  std::int64_t ts_ns;
  std::uint32_t tid;
  std::uint8_t nargs;
  std::uint8_t used;
  std::uint8_t pad_[2];
  std::uint8_t args[kLogRecordBytes - 24];
};
static_assert(sizeof(LogRecord) == kLogRecordBytes && std::is_trivially_copyable_v<LogRecord>);

using LogRing = SpscRing<LogRecord, kLogRingSize>;

// Wrap a value to have it rendered as "***".
template <class T>
struct Secret {
  T value;
};
template <class T>
Secret<std::decay_t<T>> secret(T&& v) {
  return Secret<std::decay_t<T>>{std::forward<T>(v)};
}

// Global level (runtime); FASTMM_MIN_LOG_LEVEL is the compile-time floor.
inline std::atomic<std::uint8_t> g_log_level{static_cast<std::uint8_t>(LogLevel::Info)};
[[nodiscard]] FASTMM_FORCE_INLINE bool log_enabled(LogLevel lvl) noexcept {
  return static_cast<std::uint8_t>(lvl) >= g_log_level.load(std::memory_order_relaxed);
}

// Process-wide registry of per-thread rings + the sink thread. Implemented in log_sink.cpp.
class Logger {
 public:
  static Logger& instance() noexcept;

  void set_level(LogLevel lvl) noexcept { g_log_level.store(static_cast<std::uint8_t>(lvl)); }
  [[nodiscard]] LogLevel level() const noexcept {
    return static_cast<LogLevel>(g_log_level.load());
  }

  // Allocates (heap) and registers a ring for the calling thread. Call during warm-up on
  // every thread that logs; it is otherwise done lazily on the first log statement.
  LogRing* attach_current_thread();

  // Sink control. `out` defaults to stderr. Records at >= mirror_level are also copied to
  // stderr when `out` is a different stream.
  void start(std::FILE* out = stderr, LogLevel mirror_level = LogLevel::Warn);
  void stop();  // drains everything then joins the sink thread
  // Drains all rings and fflush()es. Works with or without a running sink thread.
  void flush();
  [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }

  [[nodiscard]] std::uint64_t dropped() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
  }
  void note_drop() noexcept { dropped_.fetch_add(1, std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t written() const noexcept {
    return written_.load(std::memory_order_relaxed);
  }

  // Formats one record into `out` (appends). Public for tests/tools.
  static void format_record(const LogRecord& r, std::string& out);

 private:
  Logger() = default;
  ~Logger();
  std::size_t drain_all();  // returns records processed
  void sink_loop();

  struct Impl;
  Impl* impl_ = nullptr;
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> dropped_{0};
  std::atomic<std::uint64_t> written_{0};
};

namespace detail {

inline thread_local LogRing* t_log_ring = nullptr;
inline thread_local std::uint32_t t_log_tid = 0;

// Compile-time floor check, written as a function so -Wtype-limits stays quiet when the
// floor is 0.
constexpr bool log_compiled(LogLevel l) noexcept {
  const int v = static_cast<int>(l);
  const int floor = FASTMM_MIN_LOG_LEVEL;
  return v >= floor;
}

template <class E>
concept EnumWithToString = std::is_enum_v<E> && requires(E e) {
  { to_string(e) } -> std::convertible_to<std::string_view>;
};
template <class T>
concept StringLike = std::convertible_to<const T&, std::string_view>;
template <class T>
struct IsSecret : std::false_type {};
template <class T>
struct IsSecret<Secret<T>> : std::true_type {};
template <class T>
struct IsFixed : std::false_type {};
template <class Tag>
struct IsFixed<Fixed<Tag>> : std::true_type {};

// Type the sink hands to fmt for a given argument (used for compile-time format checks).
template <class T>
struct LogFmtTypeImpl {
  using D = std::remove_cvref_t<T>;
  using type = std::conditional_t<
      std::is_same_v<D, bool>,
      bool,
      std::conditional_t<
          std::is_same_v<D, char>,
          char,
          std::conditional_t<
              std::is_integral_v<D>,
              std::conditional_t<std::is_signed_v<D>, std::int64_t, std::uint64_t>,
              std::conditional_t<
                  std::is_floating_point_v<D>,
                  double,
                  std::conditional_t<
                      IsFixed<D>::value || IsSecret<D>::value || StringLike<D> ||
                          EnumWithToString<D>,
                      std::string_view,
                      std::conditional_t<std::is_enum_v<D>,
                                         std::int64_t,
                                         std::conditional_t<std::is_same_v<D, Timestamp> ||
                                                                std::is_same_v<D, Duration>,
                                                            std::int64_t,
                                                            void>>>>>>>;
  static_assert(!std::is_void_v<type>, "unsupported log argument type");
};
template <class T>
using LogFmtType = typename LogFmtTypeImpl<T>::type;

struct ArgPacker {
  std::uint8_t* p;
  std::uint8_t* end;
  std::uint8_t n = 0;

  FASTMM_FORCE_INLINE bool put_scalar(LogArgType t, std::uint64_t bits) noexcept {
    if (end - p < 9) return false;
    *p++ = static_cast<std::uint8_t>(t);
    std::memcpy(p, &bits, 8);
    p += 8;
    ++n;
    return true;
  }
  FASTMM_FORCE_INLINE bool put_str(LogArgType t, std::string_view s) noexcept {
    const std::size_t room = static_cast<std::size_t>(end - p);
    if (room < 2) return false;
    std::size_t len = s.size() < kLogMaxStrBytes ? s.size() : kLogMaxStrBytes;
    if (len > room - 2) len = room - 2;
    *p++ = static_cast<std::uint8_t>(t);
    *p++ = static_cast<std::uint8_t>(len);
    std::memcpy(p, s.data(), len);
    p += len;
    ++n;
    return true;
  }
};

template <class T>
FASTMM_FORCE_INLINE bool pack_arg(ArgPacker& pk, const T& v) noexcept {
  using D = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<D, bool>) {
    return pk.put_scalar(LogArgType::Bool, v ? 1U : 0U);
  } else if constexpr (std::is_same_v<D, char>) {
    return pk.put_scalar(LogArgType::Char, static_cast<std::uint8_t>(v));
  } else if constexpr (std::is_integral_v<D>) {
    if constexpr (std::is_signed_v<D>) {
      return pk.put_scalar(LogArgType::Int,
                           static_cast<std::uint64_t>(static_cast<std::int64_t>(v)));
    } else {
      return pk.put_scalar(LogArgType::Uint, static_cast<std::uint64_t>(v));
    }
  } else if constexpr (std::is_floating_point_v<D>) {
    const double d = static_cast<double>(v);
    std::uint64_t bits = 0;
    std::memcpy(&bits, &d, 8);
    return pk.put_scalar(LogArgType::Double, bits);
  } else if constexpr (std::is_same_v<D, Price>) {
    return pk.put_scalar(LogArgType::PriceFx, static_cast<std::uint64_t>(v.raw));
  } else if constexpr (std::is_same_v<D, Qty>) {
    return pk.put_scalar(LogArgType::QtyFx, static_cast<std::uint64_t>(v.raw));
  } else if constexpr (std::is_same_v<D, Notional>) {
    return pk.put_scalar(LogArgType::NotionalFx, static_cast<std::uint64_t>(v.raw));
  } else if constexpr (IsSecret<D>::value) {
    return pk.put_str(LogArgType::Secret, "***");
  } else if constexpr (EnumWithToString<D>) {
    return pk.put_str(LogArgType::Str, std::string_view(to_string(v)));
  } else if constexpr (std::is_enum_v<D>) {
    return pk.put_scalar(LogArgType::Int,
                         static_cast<std::uint64_t>(
                             static_cast<std::int64_t>(static_cast<std::underlying_type_t<D>>(v))));
  } else if constexpr (std::is_same_v<D, Timestamp> || std::is_same_v<D, Duration>) {
    return pk.put_scalar(LogArgType::Int, static_cast<std::uint64_t>(v.ns));
  } else if constexpr (StringLike<D>) {
    return pk.put_str(LogArgType::Str, std::string_view(v));
  } else {
    static_assert(sizeof(D) == 0, "unsupported log argument type");
    return false;
  }
}

template <class... Args>
FASTMM_FORCE_INLINE void log_emit(const LogDescriptor& d, const Args&... args) noexcept {
  LogRing* ring = t_log_ring;
  if (FASTMM_UNLIKELY(ring == nullptr)) {
    ring = Logger::instance().attach_current_thread();
    if (ring == nullptr) {
      Logger::instance().note_drop();
      return;
    }
  }
  LogRecord* r = ring->try_reserve();
  if (FASTMM_UNLIKELY(r == nullptr)) {
    Logger::instance().note_drop();
    return;
  }
  r->desc = &d;
  r->ts_ns = wall_now().ns;
  r->tid = t_log_tid;
  ArgPacker pk{r->args, r->args + sizeof(r->args)};
  (static_cast<void>(pack_arg(pk, args)), ...);
  r->nargs = pk.n;
  r->used = static_cast<std::uint8_t>(pk.p - r->args);
  ring->commit();
}

}  // namespace detail
}  // namespace fastmm

// fmt_str must be a string literal. The generic lambda lets us spell the packed argument
// types for the compile-time format check without touching the arguments' values.
#define FASTMM_LOG_IMPL(lvl, fmt_str, ...)                                                \
  do {                                                                                    \
    if constexpr (::fastmm::detail::log_compiled(lvl)) {                                  \
      if (::fastmm::log_enabled(lvl)) {                                                   \
        static constexpr ::fastmm::LogDescriptor fastmm_log_desc_{                        \
            fmt_str, __FILE__, __LINE__, lvl};                                            \
        [&](const auto&... fastmm_a_) {                                                   \
          static_cast<void>(                                                              \
              ::fmt::format_string<::fastmm::detail::LogFmtType<decltype(fastmm_a_)>...>( \
                  fmt_str));                                                              \
          ::fastmm::detail::log_emit(fastmm_log_desc_, fastmm_a_...);                     \
        }(__VA_ARGS__);                                                                   \
      }                                                                                   \
    }                                                                                     \
  } while (0)

#define FASTMM_LOG_TRACE(fmt_str, ...) \
  FASTMM_LOG_IMPL(::fastmm::LogLevel::Trace, fmt_str, ##__VA_ARGS__)
#define FASTMM_LOG_DEBUG(fmt_str, ...) \
  FASTMM_LOG_IMPL(::fastmm::LogLevel::Debug, fmt_str, ##__VA_ARGS__)
#define FASTMM_LOG_INFO(fmt_str, ...) \
  FASTMM_LOG_IMPL(::fastmm::LogLevel::Info, fmt_str, ##__VA_ARGS__)
#define FASTMM_LOG_WARN(fmt_str, ...) \
  FASTMM_LOG_IMPL(::fastmm::LogLevel::Warn, fmt_str, ##__VA_ARGS__)
#define FASTMM_LOG_ERROR(fmt_str, ...) \
  FASTMM_LOG_IMPL(::fastmm::LogLevel::Error, fmt_str, ##__VA_ARGS__)
