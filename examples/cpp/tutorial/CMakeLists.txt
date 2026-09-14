# The tutorial "Your first market maker" (docs/tutorials/first-strategy/), built with FastMM.
# The programs go to <build>/bin next to FastMM's own; the tests carry the ctest label `tutorial`.

# [start:cmake]
# The strategy library: the strategy header and its registration file.
add_library(tutorial_strategies STATIC strategies.cpp)
target_include_directories(tutorial_strategies PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tutorial_strategies PUBLIC fastmm::sim)

# Unit test and C++ backtest: no registration needed.
add_executable(first_mm_test first_mm_test.cpp)
target_link_libraries(first_mm_test PRIVATE tutorial_strategies)
add_executable(first_mm_backtest first_mm_backtest.cpp)
target_link_libraries(first_mm_backtest PRIVATE tutorial_strategies fastmm::backtest)

# FastMM's command lines with the tutorial's strategies registered.
add_executable(tutorial-backtest tutorial_backtest.cpp)
target_link_libraries(tutorial-backtest PRIVATE tutorial_strategies fastmm::backtest)
add_executable(tutorial-replay tutorial_replay.cpp)
target_link_libraries(tutorial-replay PRIVATE tutorial_strategies fastmm::backtest)
if(TARGET fastmm::live)
  add_executable(tutorial-live tutorial_live.cpp)
  target_link_libraries(tutorial-live PRIVATE tutorial_strategies fastmm::live)
endif()
# [end:cmake]

foreach(_t tutorial_strategies first_mm_test first_mm_backtest tutorial-backtest tutorial-replay tutorial-live)
  if(TARGET ${_t})
    fastmm_target_defaults(${_t})
    set_target_properties(${_t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
  endif()
endforeach()

if(FASTMM_BUILD_TESTS)
  add_test(NAME tutorial.first_mm_test COMMAND first_mm_test)
  add_test(NAME tutorial.first_mm_backtest COMMAND first_mm_backtest)
  set_tests_properties(tutorial.first_mm_test tutorial.first_mm_backtest PROPERTIES LABELS "tutorial;examples")
  # The tutorial's shell steps up to the simulated exchange (ports 9080 and 9443, about 90 s).
  # Not under sanitizers: the engine and the simulator run for real time.
  if(TARGET tutorial-live AND TARGET fastmm-sim-exchange AND NOT FASTMM_ENABLE_ASAN AND NOT FASTMM_ENABLE_TSAN)
    find_program(FASTMM_BASH bash)
    if(FASTMM_BASH)
      add_test(NAME tutorial.script
        COMMAND ${FASTMM_BASH} ${PROJECT_SOURCE_DIR}/scripts/docs/tutorial.sh --through sim --skip-build
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
      set_tests_properties(tutorial.script PROPERTIES
        LABELS "tutorial" RUN_SERIAL TRUE TIMEOUT 600
        ENVIRONMENT "FASTMM_BUILD_DIR=${CMAKE_BINARY_DIR}"
        DEPENDS "tutorial.first_mm_test;tutorial.first_mm_backtest")
    endif()
  endif()
endif()
