# A strategy project on top of FastMM: one strategy library registered for every runtime, and
# live / backtest / replay apps that are FastMM's command lines plus these strategies. Copy this
# directory to start your own project (docs/how-to/strategies/register-a-strategy.md).
#
#   # against an installed FastMM (cmake --install <fastmm build> --prefix <prefix>)
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<prefix>
#   # or built together with a FastMM source tree (add_subdirectory; FetchContent works the same way)
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFASTMM_SOURCE_DIR=<FastMM checkout>
#   cmake --build build && ctest --test-dir build
#
# Use the compiler FastMM was built with: a Release install holds GCC LTO objects.
cmake_minimum_required(VERSION 3.25)
project(mm_strategies LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FASTMM_SOURCE_DIR "" CACHE PATH "FastMM source tree to build with add_subdirectory (empty: find_package)")
if(FASTMM_SOURCE_DIR)
  add_subdirectory(${FASTMM_SOURCE_DIR} fastmm EXCLUDE_FROM_ALL)
else()
  find_package(fastmm CONFIG REQUIRED COMPONENTS live)
endif()

# The strategy library: the strategy headers and the one file that registers them. It needs only
# fastmm::sim (Sim and Replay factories; the Live factory uses core types), so the same library can
# go into a backtest-only build. fastmm::lowlatency compiles the engines with FastMM's code
# generation flags (x86-64-v2 baseline, frame pointers), like FastMM's own instantiations.
add_library(mm_strategies STATIC src/strategies.cpp)
target_include_directories(mm_strategies PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mm_strategies PUBLIC fastmm::sim PRIVATE fastmm::lowlatency)

add_executable(mm-live apps/live.cpp)
target_link_libraries(mm-live PRIVATE mm_strategies fastmm::live)
add_executable(mm-backtest apps/backtest.cpp)
target_link_libraries(mm-backtest PRIVATE mm_strategies fastmm::backtest)
add_executable(mm-replay apps/replay.cpp)
target_link_libraries(mm-replay PRIVATE mm_strategies fastmm::backtest)

# Unit test of the strategy's hooks with fastmm::sim::StrategyHarness; no test framework needed.
enable_testing()
add_executable(mm-strategy-test tests/microprice_mm_test.cpp)
target_link_libraries(mm-strategy-test PRIVATE mm_strategies)
add_test(NAME microprice_mm COMMAND mm-strategy-test)
