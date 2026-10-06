#pragma once
// Host settings a live process holds or inspects at start: the CPU wake-up latency request
// (/dev/cpu_dma_latency).
#include <cstdint>

namespace fastmm {

inline constexpr const char* kCpuDmaLatencyPath = "/dev/cpu_dma_latency";

// A PM QoS CPU latency request: while the file descriptor is open, the kernel keeps every CPU out
// of idle states whose exit latency exceeds the requested value; 0 keeps them polling in C0.
// Closing the descriptor (close() or the destructor) withdraws the request.
class CpuLatencyRequest {
 public:
  CpuLatencyRequest() noexcept = default;
  ~CpuLatencyRequest() { close(); }
  CpuLatencyRequest(const CpuLatencyRequest&) = delete;
  CpuLatencyRequest& operator=(const CpuLatencyRequest&) = delete;
  CpuLatencyRequest(CpuLatencyRequest&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
  CpuLatencyRequest& operator=(CpuLatencyRequest&& o) noexcept {
    if (this != &o) {
      close();
      fd_ = o.fd_;
      o.fd_ = -1;
    }
    return *this;
  }

  // Opens `path` and writes `max_us` as a 32-bit integer; replaces a request this object held.
  // Returns 0, or the errno of the open or write (ENOENT: no such device; EACCES: the device is
  // root's, 0600 by default). max_us < 0 is EINVAL.
  int open(std::int32_t max_us, const char* path = kCpuDmaLatencyPath) noexcept;
  void close() noexcept;
  [[nodiscard]] bool active() const noexcept { return fd_ >= 0; }

 private:
  int fd_ = -1;
};

}  // namespace fastmm
