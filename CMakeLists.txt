cmake_minimum_required(VERSION 3.25)

project(fastmm
  VERSION 0.1.0
  DESCRIPTION "Ultra-low-latency multi-asset market-making engine"
  HOMEPAGE_URL "https://github.com/ziyangliu-666/FastMM"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(FASTMM_BUILD_TESTS        "Build unit/property/integration tests" ${PROJECT_IS_TOP_LEVEL})
option(FASTMM_BUILD_BENCH        "Build Google Benchmark micro-benchmarks" ${PROJECT_IS_TOP_LEVEL})
option(FASTMM_BUILD_APPS         "Build fastmm-live/backtest/sim-exchange/replay" ${PROJECT_IS_TOP_LEVEL})
option(FASTMM_BUILD_EXAMPLES     "Build examples" ${PROJECT_IS_TOP_LEVEL})
option(FASTMM_BUILD_NET          "Build networking + venues + sim server (needs OpenSSL)" ON)
option(FASTMM_BUILD_PYTHON       "Build pybind11 module" OFF)
option(FASTMM_ENABLE_ASAN        "AddressSanitizer" OFF)
option(FASTMM_ENABLE_UBSAN       "UndefinedBehaviorSanitizer" OFF)
option(FASTMM_ENABLE_TSAN        "ThreadSanitizer" OFF)
option(FASTMM_ENABLE_COVERAGE    "gcov coverage" OFF)
option(FASTMM_ENABLE_LTO         "Link-time optimisation in Release" ON)
option(FASTMM_NATIVE_ARCH        "-march=native (benchmarks only; non-portable)" OFF)
option(FASTMM_WERROR             "Treat warnings as errors" OFF)
option(FASTMM_HOT_PATH_NOALLOC_CHECK "Compile no-allocation assertions into hot paths" OFF)
option(FASTMM_INSTALL            "Generate install/export rules" ${PROJECT_IS_TOP_LEVEL})
option(FASTMM_USE_CCACHE         "Use ccache if available" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  set(FASTMM_ENABLE_LTO OFF)
endif()
set(FASTMM_PIC OFF)
if(FASTMM_BUILD_PYTHON)
  set(FASTMM_PIC ON)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ${FASTMM_PIC})

include(CompilerFlags)
include(Sanitizers)
include(Dependencies)

# Generated version header
configure_file(include/fastmm/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/fastmm/version.hpp @ONLY)

# ---------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------
add_subdirectory(src/core)
add_subdirectory(src/sim)
add_subdirectory(src/strategies)
add_subdirectory(src/backtest)
add_subdirectory(src/codecs)
if(FASTMM_BUILD_NET)
  add_subdirectory(src/net)
  add_subdirectory(src/venues)
  add_subdirectory(src/live)
endif()

# Before the examples: they register tests too (labels examples and tutorial).
if(FASTMM_BUILD_TESTS)
  include(CTest)
  enable_testing()
endif()
if(FASTMM_BUILD_APPS)
  add_subdirectory(apps)
endif()
if(FASTMM_BUILD_EXAMPLES)
  add_subdirectory(examples/cpp)
  add_subdirectory(examples/quickstart)
endif()
if(FASTMM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
if(FASTMM_BUILD_BENCH)
  add_subdirectory(bench)
endif()
if(FASTMM_BUILD_PYTHON)
  add_subdirectory(python)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
if(FASTMM_INSTALL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  set(_fastmm_export_targets fastmm_core fastmm_sim fastmm_strategies fastmm_backtest fastmm_codecs fastmm_warnings fastmm_lowlatency)
  if(FASTMM_BUILD_NET)
    list(APPEND _fastmm_export_targets fastmm_net fastmm_venues fastmm_live)
  endif()
  install(TARGETS ${_fastmm_export_targets} EXPORT fastmmTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(DIRECTORY include/fastmm DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.hpp")
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/generated/fastmm/version.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fastmm)
  install(EXPORT fastmmTargets NAMESPACE fastmm:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fastmm)
  configure_package_config_file(cmake/fastmmConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/fastmmConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fastmm)
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/fastmmConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/fastmmConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/fastmmConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fastmm)
endif()

message(STATUS "fastmm ${PROJECT_VERSION}: type=${CMAKE_BUILD_TYPE} tests=${FASTMM_BUILD_TESTS} bench=${FASTMM_BUILD_BENCH} net=${FASTMM_BUILD_NET} python=${FASTMM_BUILD_PYTHON} lto=${FASTMM_ENABLE_LTO} native=${FASTMM_NATIVE_ARCH}")
