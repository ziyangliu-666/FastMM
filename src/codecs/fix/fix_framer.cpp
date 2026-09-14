#include "fastmm/codecs/fix/fix_framer.hpp"

#include "fastmm/codecs/fix/fix_tags.hpp"

#include <cstring>

namespace fastmm::codecs::fix {

namespace {

constexpr char kPrefix[] = "8=FIX";
constexpr std::size_t kPrefixLen = 5;
constexpr std::size_t kMaxBeginString = 32;
constexpr std::size_t kMaxBodyLengthDigits = 7;

enum class Probe : std::uint8_t { Frame, NeedMore, Bad };

// Structure check of a candidate starting at p[0] == "8=FIX". On Frame, `total` is its length.
Probe probe(const char* p, std::size_t n, std::size_t max_message, std::size_t& total) noexcept {
  // BeginString value up to SOH.
  const std::size_t bs_scan = n < kMaxBeginString ? n : kMaxBeginString;
  const auto* soh = static_cast<const char*>(std::memchr(p, kSoh, bs_scan));
  if (soh == nullptr) return n < kMaxBeginString ? Probe::NeedMore : Probe::Bad;
  std::size_t i = static_cast<std::size_t>(soh - p) + 1;
  if (i + 2 > n) return Probe::NeedMore;
  if (p[i] != '9' || p[i + 1] != '=') return Probe::Bad;
  std::size_t j = i + 2;
  std::size_t body_len = 0;
  while (j < n && p[j] >= '0' && p[j] <= '9') {
    if (j - (i + 2) >= kMaxBodyLengthDigits) return Probe::Bad;
    body_len = body_len * 10 + static_cast<std::size_t>(p[j] - '0');
    ++j;
  }
  if (j == n) return Probe::NeedMore;
  if (j == i + 2 || p[j] != kSoh || body_len == 0) return Probe::Bad;
  const std::size_t body_start = j + 1;
  total = body_start + body_len + 7;
  if (total > max_message) return Probe::Bad;
  if (n < total) return Probe::NeedMore;
  const std::size_t t = body_start + body_len;
  if (p[t - 1] != kSoh || p[t] != '1' || p[t + 1] != '0' || p[t + 2] != '=' || p[total - 1] != kSoh)
    return Probe::Bad;
  for (std::size_t k = t + 3; k < t + 6; ++k) {
    if (p[k] < '0' || p[k] > '9') return Probe::Bad;
  }
  return Probe::Frame;
}

// First position >= from where "8=FIX" starts, or where a proper prefix of it ends the input.
std::size_t find_start(const char* p, std::size_t n, std::size_t from) noexcept {
  for (std::size_t i = from; i < n; ++i) {
    if (p[i] != '8') continue;
    const std::size_t avail = n - i < kPrefixLen ? n - i : kPrefixLen;
    if (std::memcmp(p + i, kPrefix, avail) == 0) return i;
  }
  return n;
}

}  // namespace

FrameView FixFramer::next(std::span<const std::byte> in) noexcept {
  const auto* p = reinterpret_cast<const char*>(in.data());
  const std::size_t n = in.size();
  std::size_t start = find_start(p, n, 0);
  for (;;) {
    if (start >= n) break;
    if (n - start < kPrefixLen) break;  // partial prefix at the end: keep it
    std::size_t total = 0;
    const Probe r = probe(p + start, n - start, max_message_, total);
    if (r == Probe::Frame) {
      ++frames_;
      garbage_bytes_ += start;
      return FrameView{in.subspan(start, total), start + total, kMessage};
    }
    if (r == Probe::NeedMore) break;
    start = find_start(p, n, start + 1);
  }
  if (start > 0) {
    garbage_bytes_ += start;
    return FrameView{{}, start, kGarbage};
  }
  return {};
}

}  // namespace fastmm::codecs::fix
