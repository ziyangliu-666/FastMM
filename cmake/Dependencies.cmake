# All third-party dependencies, pinned. Fetched by CPM at configure time.
# Cache: set CPM_SOURCE_CACHE (bootstrap.sh exports ~/.cache/CPM).
include(${CMAKE_CURRENT_LIST_DIR}/CPM.cmake)

set(CPM_USE_LOCAL_PACKAGES OFF)

# --- System dependencies ---------------------------------------------------
find_package(Threads REQUIRED)

if(FASTMM_BUILD_NET)
  # FASTMM_OPENSSL_STATIC: libssl.a and libcrypto.a, e.g. from scripts/wheels/build-openssl.sh with
  # OPENSSL_ROOT_DIR pointing at its prefix.
  if(FASTMM_OPENSSL_STATIC)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
  endif()
  find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)
  if(FASTMM_OPENSSL_STATIC AND NOT (OPENSSL_SSL_LIBRARY MATCHES "\\.a$" AND OPENSSL_CRYPTO_LIBRARY MATCHES "\\.a$"))
    message(FATAL_ERROR "FASTMM_OPENSSL_STATIC: found ${OPENSSL_SSL_LIBRARY} and ${OPENSSL_CRYPTO_LIBRARY}, "
      "not static libraries; set OPENSSL_ROOT_DIR to an OpenSSL built with scripts/wheels/build-openssl.sh")
  endif()
  find_package(ZLIB REQUIRED)
endif()

# --- fmt (core: logging / formatting) --------------------------------------
CPMAddPackage(
  NAME fmt
  GITHUB_REPOSITORY fmtlib/fmt
  GIT_TAG 12.2.0
  SYSTEM YES
  OPTIONS "FMT_INSTALL ON" "FMT_DOC OFF" "FMT_TEST OFF")

# --- toml++ (core: config, header-only) ------------------------------------
CPMAddPackage(
  NAME tomlplusplus
  GITHUB_REPOSITORY marzer/tomlplusplus
  GIT_TAG v3.4.0
  SYSTEM YES
  OPTIONS "TOMLPP_BUILD_EXAMPLES OFF")

# --- simdjson (venues/sim: JSON) -------------------------------------------
if(FASTMM_BUILD_NET)
  CPMAddPackage(
    NAME simdjson
    GITHUB_REPOSITORY simdjson/simdjson
    GIT_TAG v4.6.11
    SYSTEM YES
    OPTIONS "SIMDJSON_DEVELOPER_MODE OFF" "SIMDJSON_BUILD_STATIC_LIB ON" "SIMDJSON_INSTALL ON"
            "SIMDJSON_ENABLE_THREADS OFF" "BUILD_SHARED_LIBS OFF")
endif()

# --- doctest (tests) --------------------------------------------------------
if(FASTMM_BUILD_TESTS)
  CPMAddPackage(
    NAME doctest
    GITHUB_REPOSITORY doctest/doctest
    GIT_TAG v2.5.3
    SYSTEM YES
    OPTIONS "DOCTEST_WITH_TESTS OFF" "DOCTEST_NO_INSTALL ON")
  include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
endif()

# --- Google Benchmark (bench) ----------------------------------------------
if(FASTMM_BUILD_BENCH)
  CPMAddPackage(
    NAME benchmark
    GITHUB_REPOSITORY google/benchmark
    GIT_TAG v1.9.5
    SYSTEM YES
    OPTIONS "BENCHMARK_ENABLE_TESTING OFF" "BENCHMARK_ENABLE_GTEST_TESTS OFF"
            "BENCHMARK_ENABLE_INSTALL OFF" "BENCHMARK_ENABLE_WERROR OFF"
            "BENCHMARK_INSTALL_DOCS OFF")
endif()

# --- pybind11 (python) ------------------------------------------------------
if(FASTMM_BUILD_PYTHON OR FASTMM_BUILD_PYTHON_LIVE)
  find_package(Python 3.9 REQUIRED COMPONENTS Interpreter Development.Module)
  CPMAddPackage(
    NAME pybind11
    GITHUB_REPOSITORY pybind/pybind11
    GIT_TAG v3.1.0
    SYSTEM YES
    OPTIONS "PYBIND11_INSTALL OFF" "PYBIND11_TEST OFF")
endif()
