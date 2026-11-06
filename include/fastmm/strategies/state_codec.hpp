#pragma once
// StateWriter and StateReader: the bytes of a strategy's state() and restore() without allocating
// or throwing. The writer fills a buffer the strategy owns and sizes once, so a checkpoint costs a
// copy of the state and nothing else; a write that does not fit fails the writer instead of
// growing it. Values are copied in the machine's own layout: trivially copyable types only, read
// back in the order they were written. The strategy's state_version (strategies/hooks.hpp) names
// the layout.
//
//   StateWriter w(buf_);                      // std::span<std::byte> sized in on_start
//   w.put(avg_);
//   w.put_array(std::span<const Slot>(slots_.data(), n_));
//   return w.view();                          // empty when the buffer was too small
//
//   StateReader r(bytes);
//   return r.get(avg_) && r.get_array(slots_.data(), slots_.size(), n_) && r.done();
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

namespace fastmm {

class StateWriter {
 public:
  StateWriter() = default;
  explicit StateWriter(std::span<std::byte> buf) noexcept : buf_(buf) {}

  template <class T>
    requires std::is_trivially_copyable_v<T>
  void put(const T& v) noexcept {
    put_bytes(&v, sizeof(T));
  }
  // A count (u32) and the elements.
  template <class T>
    requires std::is_trivially_copyable_v<T>
  void put_array(std::span<const T> v) noexcept {
    put(static_cast<std::uint32_t>(v.size()));
    put_bytes(v.data(), v.size_bytes());
  }
  void put_bytes(const void* p, std::size_t n) noexcept {
    if (!ok_ || n > buf_.size() - n_) {
      ok_ = false;
      return;
    }
    if (n != 0) std::memcpy(buf_.data() + n_, p, n);
    n_ += n;
  }

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] std::size_t size() const noexcept { return n_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return buf_.size(); }
  // What was written; empty after a write that did not fit.
  [[nodiscard]] std::string_view view() const noexcept {
    if (!ok_) return {};
    return {reinterpret_cast<const char*>(buf_.data()), n_};
  }

 private:
  std::span<std::byte> buf_;
  std::size_t n_ = 0;
  bool ok_ = true;
};

class StateReader {
 public:
  explicit StateReader(std::string_view bytes) noexcept : bytes_(bytes) {}

  template <class T>
    requires std::is_trivially_copyable_v<T>
  [[nodiscard]] bool get(T& v) noexcept {
    return get_bytes(&v, sizeof(T));
  }
  // What put_array wrote, into out[0, cap): false when it holds more than cap elements. `n` is
  // the count read.
  template <class T>
    requires std::is_trivially_copyable_v<T>
  [[nodiscard]] bool get_array(T* out, std::size_t cap, std::size_t& n) noexcept {
    std::uint32_t count = 0;
    if (!get(count) || count > cap || !get_bytes(out, std::size_t{count} * sizeof(T))) {
      ok_ = false;
      return false;
    }
    n = count;
    return true;
  }
  [[nodiscard]] bool get_bytes(void* p, std::size_t n) noexcept {
    if (!ok_ || n > bytes_.size() - at_) {
      ok_ = false;
      return false;
    }
    if (n != 0) std::memcpy(p, bytes_.data() + at_, n);
    at_ += n;
    return true;
  }

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  // Every byte was read, and every read fitted.
  [[nodiscard]] bool done() const noexcept { return ok_ && at_ == bytes_.size(); }

 private:
  std::string_view bytes_;
  std::size_t at_ = 0;
  bool ok_ = true;
};

}  // namespace fastmm
