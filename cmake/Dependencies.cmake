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

# --- SQLite (store: the queryable record of a session) ----------------------
# The amalgamation, pinned and fetched by CPM like every other dependency (ADR-0005): a trading
# host needs no -dev package, and the WAL and UPSERT behaviour the store relies on is the version
# pinned here rather than whatever the distribution ships.
enable_language(C)
CPMAddPackage(
  NAME sqlite3
  URL https://sqlite.org/2025/sqlite-amalgamation-3500400.zip
  URL_HASH SHA256=1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032
  DOWNLOAD_ONLY YES)
if(sqlite3_ADDED AND NOT TARGET sqlite3)
  add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
  set_target_properties(sqlite3 PROPERTIES
    C_STANDARD 11 C_EXTENSIONS OFF EXPORT_NAME sqlite3
    POSITION_INDEPENDENT_CODE ${FASTMM_PIC})
  # SYSTEM: the amalgamation is not compiled with fastmm::warnings and must not raise them in
  # the translation units that include sqlite3.h either.
  target_include_directories(sqlite3 SYSTEM PUBLIC $<BUILD_INTERFACE:${sqlite3_SOURCE_DIR}>)
  target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1            # the store thread writes while another process reads
    SQLITE_DQS=0                   # a double-quoted string is an identifier, never a literal
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_SHARED_CACHE
    SQLITE_USE_ALLOCA)
  target_link_libraries(sqlite3 PRIVATE ${CMAKE_DL_LIBS})
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
