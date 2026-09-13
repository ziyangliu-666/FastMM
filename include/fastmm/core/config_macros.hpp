#pragma once
// Compiler/portability macros and hot-path constants shared by every fastmm header.
//
// Conventions: macros are FASTMM_*, constants are kPascalCase. Nothing here may pull in
// heavyweight headers - this file is included by everything.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(__GNUC__) || defined(__clang__)
#define FASTMM_FORCE_INLINE inline __attribute__((always_inline))
#define FASTMM_NOINLINE __attribute__((noinline))
#define FASTMM_LIKELY(x) __builtin_expect(!!(x), 1)
#define FASTMM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define FASTMM_UNREACHABLE() __builtin_unreachable()
#define FASTMM_RESTRICT __restrict__
#else
#define FASTMM_FORCE_INLINE inline
#define FASTMM_NOINLINE
#define FASTMM_LIKELY(x) (x)
#define FASTMM_UNLIKELY(x) (x)
#define FASTMM_UNREACHABLE() std::abort()
#define FASTMM_RESTRICT
#endif

// Compile-time floor for log statements: 0=Trace 1=Debug 2=Info 3=Warn 4=Error 5=Off.
#ifndef FASTMM_MIN_LOG_LEVEL
#ifdef NDEBUG
#define FASTMM_MIN_LOG_LEVEL 1
#else
#define FASTMM_MIN_LOG_LEVEL 0
#endif
#endif

namespace fastmm {

// Zen4/Intel L1 line is 64 bytes; adjacent-line prefetch makes 128 the safe distance
// between hot fields written by different cores.
inline constexpr std::size_t kCacheLine = 64;

// 128-bit intermediates for fixed-point products (GCC/Clang extension, hidden from -Wpedantic).
__extension__ using Int128 = __int128;
__extension__ using Uint128 = unsigned __int128;

inline constexpr std::size_t kDestructiveInterference = 128;

namespace detail {
[[noreturn]] inline void assert_fail(const char* expr, const char* file, int line) noexcept {
  std::fprintf(stderr, "FASTMM_ASSERT failed: %s (%s:%d)\n", expr, file, line);
  std::abort();
}
}  // namespace detail

}  // namespace fastmm

// Debug-only assertion. Compiled out entirely under NDEBUG so it never affects hot-path
// codegen in Release; FASTMM_CHECK is the always-on variant for startup/config code.
#ifdef NDEBUG
#define FASTMM_ASSERT(cond) (static_cast<void>(0))
#else
#define FASTMM_ASSERT(cond)                   \
  (FASTMM_LIKELY(cond) ? static_cast<void>(0) \
                       : ::fastmm::detail::assert_fail(#cond, __FILE__, __LINE__))
#endif
#define FASTMM_CHECK(cond)                    \
  (FASTMM_LIKELY(cond) ? static_cast<void>(0) \
                       : ::fastmm::detail::assert_fail(#cond, __FILE__, __LINE__))

// Under -DFASTMM_HOT_PATH_NOALLOC_CHECK the hot-path invariants that are normally
// debug-only stay enabled in Release builds (used by the noalloc CI job).
#if defined(FASTMM_HOT_PATH_NOALLOC_CHECK) && FASTMM_HOT_PATH_NOALLOC_CHECK
#define FASTMM_HOT_ASSERT(cond) FASTMM_CHECK(cond)
#else
#define FASTMM_HOT_ASSERT(cond) FASTMM_ASSERT(cond)
#endif
