#pragma once
// Field parsing shared by the archive readers (binance_source.cpp, tardis_source.cpp). The
// files are hundreds of megabytes of ASCII, so these avoid allocating and never build a
// std::string per row.
#include <cstdint>
#include <string_view>

namespace fastmm::bt::csv {

// Splits on ',' into at most `max` views; returns the field count, which may exceed `max`.
inline std::size_t split(std::string_view line, std::string_view* out, std::size_t max) noexcept {
  std::size_t n = 0;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == ',') {
      if (n < max) out[n] = line.substr(start, i - start);
      ++n;
      start = i + 1;
    }
  }
  return n;
}

inline bool parse_u64(std::string_view s, std::uint64_t& out) noexcept {
  if (s.empty()) return false;
  std::uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    const auto d = static_cast<std::uint64_t>(c - '0');
    if (v > (UINT64_MAX - d) / 10) return false;
    v = v * 10 + d;
  }
  out = v;
  return true;
}

// Epoch times come in milliseconds (Binance futures, Binance spot before 2025), microseconds
// (Tardis, Binance spot since 2025) or nanoseconds; the digit count tells them apart for any
// date this century.
inline bool parse_epoch_ns(std::string_view s, std::int64_t& out) noexcept {
  std::uint64_t v = 0;
  if (!parse_u64(s, v)) return false;
  const std::int64_t scale = s.size() >= 19 ? 1 : (s.size() >= 16 ? 1'000 : 1'000'000);
  if (v > static_cast<std::uint64_t>(INT64_MAX / scale)) return false;
  out = static_cast<std::int64_t>(v) * scale;
  return true;
}

}  // namespace fastmm::bt::csv
