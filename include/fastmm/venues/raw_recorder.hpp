#pragma once
// RawRecorder: appends every raw WebSocket frame to <dir>/<venue>-<channel>.jsonl (one frame per
// line, prefixed with the receive timestamp; binary frames as hex) so fixtures can be captured
// from a live session (`fastmm-live --record-raw DIR`, plan 8.8). Off by default; when enabled it
// does buffered stdio writes on the net thread, so it is a diagnostic tool, not a hot-path
// feature.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace fastmm::venues {

class RawRecorder {
 public:
  RawRecorder() = default;
  ~RawRecorder() { close(); }
  RawRecorder(const RawRecorder&) = delete;
  RawRecorder& operator=(const RawRecorder&) = delete;

  bool open(const std::string& dir, std::string_view venue, std::string_view channel) {
    close();
    const std::string path = dir + "/" + std::string(venue) + "-" + std::string(channel) + ".jsonl";
    file_ = std::fopen(path.c_str(), "a");
    return file_ != nullptr;
  }
  void close() noexcept {
    if (file_ != nullptr) {
      std::fclose(file_);
      file_ = nullptr;
    }
  }
  [[nodiscard]] bool enabled() const noexcept { return file_ != nullptr; }

  void record(std::int64_t rx_ns, std::string_view frame) noexcept {
    if (file_ == nullptr) return;
    std::fprintf(file_, "%lld\t", static_cast<long long>(rx_ns));
    std::fwrite(frame.data(), 1, frame.size(), file_);
    std::fputc('\n', file_);
    ++frames_;
  }
  // Binary frame (SBE market data) as lowercase hex on one line, same timestamp prefix.
  void record_hex(std::int64_t rx_ns, std::span<const std::byte> frame) noexcept {
    if (file_ == nullptr) return;
    static constexpr char kHex[] = "0123456789abcdef";
    std::fprintf(file_, "%lld\t", static_cast<long long>(rx_ns));
    for (const std::byte b : frame) {
      const auto v = static_cast<unsigned>(b);
      std::fputc(kHex[v >> 4], file_);
      std::fputc(kHex[v & 15U], file_);
    }
    std::fputc('\n', file_);
    ++frames_;
  }
  void flush() noexcept {
    if (file_ != nullptr) std::fflush(file_);
  }
  [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }

 private:
  std::FILE* file_ = nullptr;
  std::uint64_t frames_ = 0;
};

}  // namespace fastmm::venues
