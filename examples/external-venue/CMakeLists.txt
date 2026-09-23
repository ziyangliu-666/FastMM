# A venue connector in a project of its own: one library holding the connector and its
# registration, and a live app that is FastMM's command line plus this venue. Copy this directory
# to start your own (docs/how-to/venues/add-a-venue.md).
#
#   # against an installed FastMM (cmake --install <fastmm build> --prefix <prefix>)
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<prefix>
#   # or built together with a FastMM source tree (add_subdirectory; FetchContent works the same way)
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFASTMM_SOURCE_DIR=<FastMM checkout>
#   cmake --build build && ctest --test-dir build
#
# Use the compiler FastMM was built with: a Release install holds GCC LTO objects.
cmake_minimum_required(VERSION 3.25)
project(echo_venue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FASTMM_SOURCE_DIR "" CACHE PATH "FastMM source tree to build with add_subdirectory (empty: find_package)")
if(FASTMM_SOURCE_DIR)
  add_subdirectory(${FASTMM_SOURCE_DIR} fastmm EXCLUDE_FROM_ALL)
else()
  find_package(fastmm CONFIG REQUIRED COMPONENTS live)
endif()

# The connector and the one file that registers it. fastmm::venues is the whole dependency: a venue
# needs the Venue interface, the registry and the networking library behind them.
add_library(echo_venue STATIC src/echo_venue.cpp src/register.cpp)
target_include_directories(echo_venue PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(echo_venue PUBLIC fastmm::venues)

add_executable(echo-live apps/live.cpp)
target_link_libraries(echo-live PRIVATE echo_venue fastmm::live)

# Registration, the keys the venue owns and the factory, without a network. No test framework.
enable_testing()
add_executable(echo-venue-test tests/echo_venue_test.cpp)
target_link_libraries(echo-venue-test PRIVATE echo_venue)
add_test(NAME echo_venue COMMAND echo-venue-test)
