#pragma once
// Minimal expected<T, E>: the hot path never throws, it returns Result.
//
//   Result<ClientOrderId, RejectReason> r = ctx.send(order);
//   if (!r) return r.error();
//   use(r.value());
//
// Errors are constructed via fastmm::fail(e) so that `Result<int, int>` is unambiguous.
#include "fastmm/core/config_macros.hpp"

#include <new>
#include <type_traits>
#include <utility>

namespace fastmm {

template <class E>
struct Unexpected {
  E error;
};

template <class E>
[[nodiscard]] constexpr Unexpected<std::decay_t<E>> fail(E&& e) noexcept {
  return Unexpected<std::decay_t<E>>{std::forward<E>(e)};
}

template <class T, class E>
class [[nodiscard]] Result {
  static_assert(!std::is_void_v<E>, "error type must not be void");
  static_assert(!std::is_reference_v<T> && !std::is_reference_v<E>);

 public:
  using value_type = T;
  using error_type = E;

  // NOLINTNEXTLINE(google-explicit-constructor): value → Result is the whole point.
  constexpr Result(const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>)
      : has_value_(true) {
    ::new (static_cast<void*>(&value_)) T(v);
  }
  // NOLINTNEXTLINE(google-explicit-constructor)
  constexpr Result(T&& v) noexcept(std::is_nothrow_move_constructible_v<T>) : has_value_(true) {
    ::new (static_cast<void*>(&value_)) T(std::move(v));
  }
  // NOLINTNEXTLINE(google-explicit-constructor)
  constexpr Result(Unexpected<E> u) noexcept(std::is_nothrow_move_constructible_v<E>)
      : has_value_(false) {
    ::new (static_cast<void*>(&error_)) E(std::move(u.error));
  }

  constexpr Result(const Result& o) noexcept(std::is_nothrow_copy_constructible_v<T> &&
                                             std::is_nothrow_copy_constructible_v<E>)
      : has_value_(o.has_value_) {
    if (has_value_) {
      ::new (static_cast<void*>(&value_)) T(o.value_);
    } else {
      ::new (static_cast<void*>(&error_)) E(o.error_);
    }
  }
  constexpr Result(Result&& o) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                        std::is_nothrow_move_constructible_v<E>)
      : has_value_(o.has_value_) {
    if (has_value_) {
      ::new (static_cast<void*>(&value_)) T(std::move(o.value_));
    } else {
      ::new (static_cast<void*>(&error_)) E(std::move(o.error_));
    }
  }
  constexpr Result& operator=(const Result& o) {
    if (this != &o) {
      destroy();
      has_value_ = o.has_value_;
      if (has_value_) {
        ::new (static_cast<void*>(&value_)) T(o.value_);
      } else {
        ::new (static_cast<void*>(&error_)) E(o.error_);
      }
    }
    return *this;
  }
  constexpr Result& operator=(Result&& o) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                                   std::is_nothrow_move_constructible_v<E>) {
    if (this != &o) {
      destroy();
      has_value_ = o.has_value_;
      if (has_value_) {
        ::new (static_cast<void*>(&value_)) T(std::move(o.value_));
      } else {
        ::new (static_cast<void*>(&error_)) E(std::move(o.error_));
      }
    }
    return *this;
  }
  ~Result() { destroy(); }

  [[nodiscard]] constexpr bool has_value() const noexcept { return has_value_; }
  constexpr explicit operator bool() const noexcept { return has_value_; }

  [[nodiscard]] constexpr T& value() & noexcept {
    FASTMM_ASSERT(has_value_);
    return value_;
  }
  [[nodiscard]] constexpr const T& value() const& noexcept {
    FASTMM_ASSERT(has_value_);
    return value_;
  }
  [[nodiscard]] constexpr T&& value() && noexcept {
    FASTMM_ASSERT(has_value_);
    return std::move(value_);
  }
  [[nodiscard]] constexpr const E& error() const noexcept {
    FASTMM_ASSERT(!has_value_);
    return error_;
  }
  [[nodiscard]] constexpr E& error() noexcept {
    FASTMM_ASSERT(!has_value_);
    return error_;
  }
  constexpr T& operator*() noexcept { return value(); }
  constexpr const T& operator*() const noexcept { return value(); }
  constexpr T* operator->() noexcept { return &value(); }
  constexpr const T* operator->() const noexcept { return &value(); }

  template <class U>
  [[nodiscard]] constexpr T value_or(U&& fallback) const {
    return has_value_ ? value_ : static_cast<T>(std::forward<U>(fallback));
  }

  // Monadic chaining: f(T) must return Result<U, E>.
  template <class F>
  [[nodiscard]] constexpr auto and_then(F&& f) const -> std::invoke_result_t<F, const T&> {
    using R = std::invoke_result_t<F, const T&>;
    static_assert(std::is_same_v<typename R::error_type, E>);
    if (has_value_) return std::forward<F>(f)(value_);
    return R(fail(error_));
  }
  // map: f(T) returns U, yields Result<U, E>.
  template <class F>
  [[nodiscard]] constexpr auto map(F&& f) const -> Result<std::invoke_result_t<F, const T&>, E> {
    using U = std::invoke_result_t<F, const T&>;
    if (has_value_) return Result<U, E>(std::forward<F>(f)(value_));
    return Result<U, E>(fail(error_));
  }

 private:
  constexpr void destroy() noexcept {
    if constexpr (!std::is_trivially_destructible_v<T> || !std::is_trivially_destructible_v<E>) {
      if (has_value_) {
        value_.~T();
      } else {
        error_.~E();
      }
    }
  }

  union {
    T value_;
    E error_;
  };
  bool has_value_;
};

// Result<void, E>: success carries nothing.
template <class E>
class [[nodiscard]] Result<void, E> {
 public:
  using value_type = void;
  using error_type = E;

  constexpr Result() noexcept : error_(), has_value_(true) {}
  // NOLINTNEXTLINE(google-explicit-constructor)
  constexpr Result(Unexpected<E> u) noexcept : error_(std::move(u.error)), has_value_(false) {}

  [[nodiscard]] constexpr bool has_value() const noexcept { return has_value_; }
  constexpr explicit operator bool() const noexcept { return has_value_; }
  [[nodiscard]] constexpr const E& error() const noexcept {
    FASTMM_ASSERT(!has_value_);
    return error_;
  }
  template <class F>
  [[nodiscard]] constexpr auto and_then(F&& f) const -> std::invoke_result_t<F> {
    using R = std::invoke_result_t<F>;
    if (has_value_) return std::forward<F>(f)();
    return R(fail(error_));
  }

 private:
  E error_;
  bool has_value_;
};

}  // namespace fastmm
