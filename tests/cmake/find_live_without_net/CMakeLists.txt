# find_package(fastmm COMPONENTS live) against a FastMM built without networking must fail with a
# message that names the missing component and FASTMM_BUILD_NET (tests/CMakeLists.txt).
cmake_minimum_required(VERSION 3.25)
project(find_live_without_net LANGUAGES NONE)
find_package(fastmm CONFIG REQUIRED COMPONENTS live)
