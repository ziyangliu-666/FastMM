#pragma once
// Thread affinity/naming and the spin-wait policy used by engine/net loops.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/time.hpp"

#include <x86intrin.h>

#include <cstdint>

namespace fastmm {

// cpu < 0 is a no-op (returns true). Implemented in src/core/thread_utils.cpp.
bool pin_to_cpu(int cpu) noexcept;
bool set_thread_name(const char* name) noexcept;  // <= 15 chars
[[nodiscard]] int current_cpu() noexcept;
[[nodiscard]] int cpu_count() noexcept;
void sleep_for(Duration d) noexcept;  // nanosleep

enum class SpinMode : std::uint8_t { Busy = 0, Adaptive = 1 };

// Busy: always _mm_pause. Adaptive (WSL2 default): pause for `spins` idle iterations, then
// nanosleep(sleep) per idle iteration until activity resets the counter.
class SpinPolicy {
 public:
  explicit SpinPolicy(SpinMode mode = SpinMode::Busy,
                      std::uint32_t spins = 20'000,
                      Duration sleep = microseconds(50)) noexcept
      : mode_(mode), spins_(spins), sleep_(sleep) {}

  FASTMM_FORCE_INLINE void idle() noexcept {
    if (mode_ == SpinMode::Busy || idle_ < spins_) {
      ++idle_;
      _mm_pause();
      return;
    }
    sleep_for(sleep_);
  }
  FASTMM_FORCE_INLINE void active() noexcept { idle_ = 0; }
  [[nodiscard]] SpinMode mode() const noexcept { return mode_; }

 private:
  SpinMode mode_;
  std::uint32_t spins_;
  std::uint32_t idle_ = 0;
  Duration sleep_;
};

}  // namespace fastmm
