# Each bench_*.cpp becomes its own executable, registered as a "bench-smoke" ctest entry that runs
# with a tiny min_time so CI verifies every benchmark builds and runs.
#
# Benchmarks link only the library they measure. A core benchmark must not depend on sim, backtest
# or networking, so a change there cannot break (or slow the link of) the core numbers.
add_library(fastmm_bench_main STATIC bench_main.cpp)
target_link_libraries(fastmm_bench_main PUBLIC benchmark::benchmark fastmm::core)
fastmm_target_defaults(fastmm_bench_main)

file(GLOB _benches CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bench_*.cpp)
list(REMOVE_ITEM _benches ${CMAKE_CURRENT_SOURCE_DIR}/bench_main.cpp)

# name regex -> extra library. First match wins; no match means core only.
set(_bench_net_regex      "^bench_(ws|http)$")
set(_bench_venues_regex   "^bench_json$")
set(_bench_backtest_regex "^bench_(matching|tick_to_order|backtest)$")

foreach(src ${_benches})
  get_filename_component(name ${src} NAME_WE)
  if(name MATCHES "${_bench_net_regex}")
    set(_lib fastmm::net)
  elseif(name MATCHES "${_bench_venues_regex}")
    set(_lib fastmm::venues)
  elseif(name MATCHES "${_bench_backtest_regex}")
    set(_lib fastmm::backtest)
  else()
    set(_lib fastmm::core)
  endif()
  if(NOT TARGET ${_lib})
    continue()  # e.g. FASTMM_BUILD_NET=OFF skips net and venue benchmarks
  endif()
  add_executable(${name} ${src})
  fastmm_target_defaults(${name})
  target_link_libraries(${name} PRIVATE fastmm_bench_main ${_lib})
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin/bench)
  if(FASTMM_BUILD_TESTS)
    add_test(NAME bench.${name}.smoke COMMAND ${name} --benchmark_min_time=0.01s --benchmark_repetitions=1)
    set_tests_properties(bench.${name}.smoke PROPERTIES LABELS "bench-smoke")
  endif()
endforeach()
