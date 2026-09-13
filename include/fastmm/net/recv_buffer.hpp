#pragma once
// Contiguous receive buffer for the hot receive path.
//
//   [0 ........ rd_ ........ wr_ ........ capacity_)[ kPadding ]
//               ^ readable() ^ writable()             ^ never written by us, always
//                                                       readable so a simdjson parser can
//                                                       overread a frame payload safely
//
// Single allocation at construction. Compaction happens in writable():
//   * rd_ == wr_            -> reset both cursors to 0 (free);
//   * tail < kCompactBelow  -> memmove unread bytes to the front (amortised: this only
//                              happens when a message straddles the end of the buffer).
// Readable data is never moved by commit()/consume(), so offsets relative to
// readable().data() stay valid until the next writable() call.
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string_view>

namespace fastmm::net {

class RecvBuffer {
 public:
  // Equals SIMDJSON_PADDING; simdjson must not be included here (net does not depend on it).
  static constexpr std::size_t kPadding = 64;
  static constexpr std::size_t kCompactBelow = 64 * 1024;
  static constexpr std::size_t kAlignment = 64;

  explicit RecvBuffer(std::size_t capacity)
      : capacity_(capacity),
        data_(static_cast<std::byte*>(
            ::operator new(capacity + kPadding, std::align_val_t{kAlignment}))) {
    std::memset(data_ + capacity_, 0, kPadding);
  }
  ~RecvBuffer() { ::operator delete(data_, std::align_val_t{kAlignment}); }
  RecvBuffer(const RecvBuffer&) = delete;
  RecvBuffer& operator=(const RecvBuffer&) = delete;
  RecvBuffer(RecvBuffer&& o) noexcept
      : capacity_(o.capacity_), data_(o.data_), rd_(o.rd_), wr_(o.wr_) {
    o.data_ = nullptr;
    o.capacity_ = o.rd_ = o.wr_ = 0;
  }
  RecvBuffer& operator=(RecvBuffer&&) = delete;

  std::size_t capacity() const noexcept { return capacity_; }
  std::size_t size() const noexcept { return wr_ - rd_; }
  bool empty() const noexcept { return rd_ == wr_; }
  std::size_t free_space() const noexcept { return capacity_ - wr_; }

  // Space to read into. Applies the compaction rule first (may move readable bytes).
  std::span<std::byte> writable() noexcept {
    if (rd_ == wr_) {
      rd_ = wr_ = 0;
    } else if (capacity_ - wr_ < kCompactBelow && rd_ > 0) {
      compact();
    }
    return {data_ + wr_, capacity_ - wr_};
  }

  // Forces the unread bytes to the front regardless of the heuristic (used when a message
  // needs more contiguous space than the tail offers).
  void compact() noexcept {
    if (rd_ == 0) return;
    const std::size_t n = wr_ - rd_;
    std::memmove(data_, data_ + rd_, n);
    rd_ = 0;
    wr_ = n;
  }

  void commit(std::size_t n) noexcept { wr_ += n; }

  std::span<std::byte> readable() noexcept { return {data_ + rd_, wr_ - rd_}; }
  std::span<const std::byte> readable() const noexcept { return {data_ + rd_, wr_ - rd_}; }
  std::string_view readable_view() const noexcept {
    return {reinterpret_cast<const char*>(data_ + rd_), wr_ - rd_};
  }

  void consume(std::size_t n) noexcept { rd_ += n; }
  void clear() noexcept { rd_ = wr_ = 0; }

  std::byte* data() noexcept { return data_; }
  std::size_t read_offset() const noexcept { return rd_; }
  std::size_t write_offset() const noexcept { return wr_; }

 private:
  std::size_t capacity_;
  std::byte* data_;
  std::size_t rd_ = 0;
  std::size_t wr_ = 0;
};

}  // namespace fastmm::net
