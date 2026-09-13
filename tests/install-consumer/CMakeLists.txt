# Downstream-project smoke test: only find_package(fastmm), no access to the source tree.
# CI installs the release build to a prefix and builds this against it.
cmake_minimum_required(VERSION 3.25)
project(fastmm_consumer LANGUAGES CXX)
find_package(fastmm CONFIG REQUIRED)
add_executable(consumer main.cpp)
# fastmm::backtest pulls in sim, core and their transitive dependencies (fmt, Threads), so a
# missing entry in the export set or in fastmmConfig.cmake shows up here as a link error.
target_link_libraries(consumer PRIVATE fastmm::backtest)
target_compile_features(consumer PRIVATE cxx_std_20)
