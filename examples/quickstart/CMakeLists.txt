# The quick start: one strategy header and a main that backtests it (docs/getting-started/quickstart.md).
#
#   cmake -S examples/quickstart -B build/quickstart -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/quickstart && ./build/quickstart/my_mm_backtest
#
# FetchContent downloads FastMM; pass -DFETCHCONTENT_SOURCE_DIR_FASTMM=<checkout> to use a local
# source tree instead. Inside FastMM's own build (examples) the fastmm:: targets already exist.
# [start:cmake]
cmake_minimum_required(VERSION 3.25)
project(my_mm LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

if(NOT TARGET fastmm::backtest)
  include(FetchContent)
  set(FASTMM_BUILD_NET OFF CACHE BOOL "backtests need no networking")
  FetchContent_Declare(fastmm GIT_REPOSITORY https://github.com/ziyangliu-666/FastMM GIT_TAG main)
  FetchContent_MakeAvailable(fastmm)
endif()

add_executable(my_mm_backtest main.cpp)
target_link_libraries(my_mm_backtest PRIVATE fastmm::backtest)
# [end:cmake]

# Inside FastMM's own build the quick start is a test: it must print a summary with fills.
if(CMAKE_SOURCE_DIR STREQUAL fastmm_SOURCE_DIR)
  fastmm_target_defaults(my_mm_backtest)
  if(FASTMM_BUILD_TESTS)
    add_test(NAME examples.quickstart COMMAND my_mm_backtest)
    set_tests_properties(examples.quickstart PROPERTIES LABELS "examples"
      PASS_REGULAR_EXPRESSION "fills \\(maker / taker\\) +[1-9]")
  endif()
endif()
