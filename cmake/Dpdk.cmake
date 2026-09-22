# DPDK for the rx_backend = "dpdk" DatagramSource (FASTMM_WITH_DPDK=ON).
#
# pkg-config's libdpdk first (a system install or PKG_CONFIG_PATH). Without one, and with
# FASTMM_DPDK_FETCH=ON, scripts/build-dpdk.sh builds a minimal static DPDK into
# <build>/_deps/dpdk at configure time (meson, ninja and pyelftools in a venv there; no root).
# Sets DPDK_STATIC_INCLUDE_DIRS, DPDK_STATIC_LDFLAGS (static link line) and FASTMM_DPDK_CFLAGS.
find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK QUIET libdpdk)
if(NOT DPDK_FOUND)
  if(NOT FASTMM_DPDK_FETCH)
    message(FATAL_ERROR "FASTMM_WITH_DPDK: pkg-config finds no libdpdk (set PKG_CONFIG_PATH or FASTMM_DPDK_FETCH=ON)")
  endif()
  set(_dpdk_prefix ${CMAKE_BINARY_DIR}/_deps/dpdk)
  if(NOT EXISTS ${_dpdk_prefix}/lib/pkgconfig/libdpdk.pc)
    message(STATUS "fastmm: building DPDK into ${_dpdk_prefix} (scripts/build-dpdk.sh, a few minutes)")
    execute_process(
      COMMAND ${PROJECT_SOURCE_DIR}/scripts/build-dpdk.sh ${_dpdk_prefix}
      OUTPUT_FILE ${CMAKE_BINARY_DIR}/_deps/dpdk-build.log
      ERROR_FILE ${CMAKE_BINARY_DIR}/_deps/dpdk-build.log
      RESULT_VARIABLE _dpdk_rc)
    if(NOT _dpdk_rc EQUAL 0)
      message(FATAL_ERROR "DPDK build failed; see ${CMAKE_BINARY_DIR}/_deps/dpdk-build.log")
    endif()
  endif()
  set(ENV{PKG_CONFIG_PATH} "${_dpdk_prefix}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
  unset(DPDK_FOUND CACHE)
  pkg_check_modules(DPDK REQUIRED libdpdk)
endif()
message(STATUS "fastmm: DPDK ${DPDK_VERSION} (${DPDK_STATIC_INCLUDE_DIRS})")

# "-include rte_config.h" would be split into two list items; the source includes it itself.
set(FASTMM_DPDK_CFLAGS ${DPDK_STATIC_CFLAGS_OTHER})
list(REMOVE_ITEM FASTMM_DPDK_CFLAGS "-include" "rte_config.h")
