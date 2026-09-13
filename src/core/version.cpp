#include "fastmm/version.hpp"

namespace fastmm {

const char* build_info() noexcept {
  return "fastmm " FASTMM_VERSION_STRING " | " FASTMM_COMPILER " | " FASTMM_BUILD_TYPE
#if FASTMM_NATIVE_ARCH
         " | -march=native"
#endif
#if FASTMM_LTO
         " | LTO"
#endif
      ;
}

}  // namespace fastmm
