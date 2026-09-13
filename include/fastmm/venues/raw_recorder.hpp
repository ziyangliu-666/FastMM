#pragma once
// RawRecorder: appends every raw WebSocket text frame to <dir>/<venue>-<channel>.jsonl (one
// frame per line, prefixed with the receive timestamp) so fixtures can be captured from a
// live session (`fastmm-live --record-raw DIR`, plan 8.8). Off by default; when enabled it
// does buffered stdio writes on the net thread, so it is a diagnostic tool, not a hot-path
// feature.
#include <cstdio>
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
  void flush() noexcept {
    if (file_ != nullptr) std::fflush(file_);
  }
  [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }

 private:
  std::FILE* file_ = nullptr;
  std::uint64_t frames_ = 0;
};

}  // namespace fastmm::venues
