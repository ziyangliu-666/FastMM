#pragma once
// InplaceCallback: a move-only void() callable stored inline, never on the heap. Reactor timers use
// it so arming a timer does not allocate. A callable larger than kCapacity bytes does not compile.
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace fastmm::net {

class InplaceCallback {
 public:
  static constexpr std::size_t kCapacity = 64;

  InplaceCallback() noexcept = default;
  template <class F>
    requires(!std::is_same_v<std::decay_t<F>, InplaceCallback> &&
             std::is_invocable_r_v<void, std::decay_t<F>&>)
  // NOLINTNEXTLINE(google-explicit-constructor,bugprone-forwarding-reference-overload)
  InplaceCallback(F&& f) noexcept(std::is_nothrow_constructible_v<std::decay_t<F>, F&&>) {
    using D = std::decay_t<F>;
    static_assert(sizeof(D) <= kCapacity, "callable too large for InplaceCallback (> 64 bytes)");
    static_assert(alignof(D) <= alignof(std::max_align_t), "over-aligned callable");
    static_assert(std::is_nothrow_move_constructible_v<D>, "callable must be nothrow movable");
    ::new (static_cast<void*>(buf_)) D(std::forward<F>(f));
    ops_ = &kOps<D>;
  }
  InplaceCallback(InplaceCallback&& o) noexcept { take(o); }
  InplaceCallback& operator=(InplaceCallback&& o) noexcept {
    if (this != &o) {
      reset();
      take(o);
    }
    return *this;
  }
  InplaceCallback(const InplaceCallback&) = delete;
  InplaceCallback& operator=(const InplaceCallback&) = delete;
  ~InplaceCallback() { reset(); }

  void operator()() { ops_->call(buf_); }
  explicit operator bool() const noexcept { return ops_ != nullptr; }
  void reset() noexcept {
    if (ops_ != nullptr) {
      ops_->destroy(buf_);
      ops_ = nullptr;
    }
  }

 private:
  struct Ops {
    void (*call)(void* self);
    void (*move)(void* dst, void* src) noexcept;  // move-constructs dst from src, destroys src
    void (*destroy)(void* self) noexcept;
  };
  template <class D>
  static constexpr Ops kOps{[](void* self) { (*static_cast<D*>(self))(); },
                            [](void* dst, void* src) noexcept {
                              ::new (dst) D(std::move(*static_cast<D*>(src)));
                              static_cast<D*>(src)->~D();
                            },
                            [](void* self) noexcept { static_cast<D*>(self)->~D(); }};

  void take(InplaceCallback& o) noexcept {
    if (o.ops_ != nullptr) {
      o.ops_->move(buf_, o.buf_);
      ops_ = std::exchange(o.ops_, nullptr);
    }
  }

  alignas(std::max_align_t) std::byte buf_[kCapacity];
  const Ops* ops_ = nullptr;
};

}  // namespace fastmm::net
