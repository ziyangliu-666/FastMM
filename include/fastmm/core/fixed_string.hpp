#pragma once
// FixedString<N>: inline, trivially copyable string with capacity N and an explicit length
// byte (sizeof == N + 1). Used for venue order ids, symbols and message payloads where
// std::string would allocate. Truncates silently on overflow (assign() reports it).
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace fastmm {

template <std::size_t N>
class FixedString {
  static_assert(N > 0 && N <= 255, "FixedString capacity must fit in a length byte");

 public:
  static constexpr std::size_t kCapacity = N;

  constexpr FixedString() noexcept : data_{}, len_(0) {}
  // NOLINTNEXTLINE(google-explicit-constructor): literals convert implicitly.
  constexpr FixedString(std::string_view sv) noexcept : data_{}, len_(0) { assign(sv); }
  // NOLINTNEXTLINE(google-explicit-constructor)
  constexpr FixedString(const char* s) noexcept : FixedString(std::string_view(s)) {}

  // Copies min(sv.size(), N) bytes; returns false if truncated.
  constexpr bool assign(std::string_view sv) noexcept {
    const std::size_t n = sv.size() < N ? sv.size() : N;
    for (std::size_t i = 0; i < n; ++i) data_[i] = sv[i];
    for (std::size_t i = n; i < N; ++i) data_[i] = '\0';
    len_ = static_cast<std::uint8_t>(n);
    return n == sv.size();
  }
  constexpr void clear() noexcept { assign({}); }
  constexpr bool push_back(char c) noexcept {
    if (len_ >= N) return false;
    data_[len_++] = c;
    return true;
  }
  constexpr bool append(std::string_view sv) noexcept {
    bool ok = true;
    for (char c : sv) ok = push_back(c) && ok;
    return ok;
  }

  [[nodiscard]] constexpr std::size_t size() const noexcept { return len_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return len_ == 0; }
  [[nodiscard]] constexpr const char* data() const noexcept { return data_; }
  [[nodiscard]] constexpr char* data() noexcept { return data_; }
  constexpr void set_size(std::size_t n) noexcept {
    len_ = static_cast<std::uint8_t>(n < N ? n : N);
  }
  [[nodiscard]] constexpr std::string_view view() const noexcept { return {data_, len_}; }
  // NOLINTNEXTLINE(google-explicit-constructor)
  constexpr operator std::string_view() const noexcept { return view(); }
  constexpr char operator[](std::size_t i) const noexcept { return data_[i]; }
  constexpr char& operator[](std::size_t i) noexcept { return data_[i]; }

  // Null-terminated view for C APIs; only valid if size() < N (always true for N-1 content).
  [[nodiscard]] const char* c_str() const noexcept {
    // data_ is zero-filled beyond len_ by assign(); push_back never writes past len_.
    return data_;
  }

  [[nodiscard]] constexpr std::uint64_t hash() const noexcept {
    std::uint64_t h = 14695981039346656037ULL;  // FNV-1a
    for (std::size_t i = 0; i < len_; ++i) {
      h ^= static_cast<std::uint8_t>(data_[i]);
      h *= 1099511628211ULL;
    }
    return h;
  }

  // Single template so `fs == fs`, `fs == "lit"` and `fs == sv` are all unambiguous under
  // C++20 rewritten-candidate rules.
  template <class R>
    requires std::convertible_to<const R&, std::string_view>
  friend constexpr bool operator==(const FixedString& a, const R& b) noexcept {
    return a.view() == std::string_view(b);
  }
  friend constexpr auto operator<=>(const FixedString& a, const FixedString& b) noexcept {
    return a.view() <=> b.view();
  }

 private:
  char data_[N];
  std::uint8_t len_;
};

static_assert(std::is_trivially_copyable_v<FixedString<16>>);
static_assert(sizeof(FixedString<16>) == 17);

}  // namespace fastmm
