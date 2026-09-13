#pragma once
// In-memory ByteStream for tests: two MemoryPipes connected back-to-back give a client and a
// server end that exchange bytes without sockets. Chunk limits simulate partial reads/writes.
// Not for production use (unbounded std::vector storage).
#include "fastmm/net/byte_stream.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace fastmm::net {

struct MemoryPipe {
  std::vector<std::byte> data;
  std::size_t rd = 0;
  bool closed = false;  // writer end closed: readers see EOF once drained
  // Simulation knobs.
  std::size_t max_read_chunk = std::numeric_limits<std::size_t>::max();
  std::size_t max_write_chunk = std::numeric_limits<std::size_t>::max();
  std::size_t capacity = std::numeric_limits<std::size_t>::max();  // want_write beyond this

  std::size_t pending() const noexcept { return data.size() - rd; }
  void compact() {
    if (rd == data.size()) {
      data.clear();
      rd = 0;
    } else if (rd > (1u << 16)) {
      data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(rd));
      rd = 0;
    }
  }
};

class MemoryTransport {
 public:
  MemoryTransport() = default;
  MemoryTransport(MemoryPipe& in, MemoryPipe& out) noexcept : in_(&in), out_(&out) {}

  IoResult handshake() noexcept { return IoResult::done(0); }

  IoResult read(std::span<std::byte> buf) noexcept {
    if (in_ == nullptr || closed_) return IoResult::failure(NetError::InvalidState);
    const std::size_t avail = in_->pending();
    if (avail == 0) return in_->closed ? IoResult::eof() : IoResult::wants_read();
    const std::size_t n = std::min({avail, buf.size(), in_->max_read_chunk});
    if (n == 0) return IoResult::done(0);
    std::memcpy(buf.data(), in_->data.data() + in_->rd, n);
    in_->rd += n;
    in_->compact();
    return IoResult::done(n);
  }

  IoResult write(std::span<const std::byte> buf) noexcept {
    if (out_ == nullptr || closed_) return IoResult::failure(NetError::InvalidState);
    if (out_->closed) return IoResult::eof();
    const std::size_t room =
        out_->capacity > out_->pending() ? out_->capacity - out_->pending() : 0;
    const std::size_t n = std::min({buf.size(), room, out_->max_write_chunk});
    out_->data.insert(out_->data.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
    if (n < buf.size()) return IoResult::wants_write(n);
    return IoResult::done(n);
  }

  int fd() const noexcept { return -1; }
  void close() noexcept {
    if (out_ != nullptr) out_->closed = true;
    closed_ = true;
  }
  bool is_open() const noexcept { return !closed_; }

 private:
  MemoryPipe* in_ = nullptr;
  MemoryPipe* out_ = nullptr;
  bool closed_ = false;
};

static_assert(ByteStream<MemoryTransport>);

// Two connected transports sharing a pair of pipes.
struct MemoryLink {
  MemoryPipe a_to_b;
  MemoryPipe b_to_a;
  MemoryTransport end_a() noexcept { return MemoryTransport(b_to_a, a_to_b); }
  MemoryTransport end_b() noexcept { return MemoryTransport(a_to_b, b_to_a); }
};

}  // namespace fastmm::net
