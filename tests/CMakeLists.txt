# One test executable per library; doctest subcases inside. Labels drive ctest presets.

add_library(fastmm_test_support STATIC support/doctest_main.cpp)
target_link_libraries(fastmm_test_support PUBLIC doctest::doctest fastmm::core)
target_include_directories(fastmm_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_compile_definitions(fastmm_test_support PUBLIC
  FASTMM_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  FASTMM_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tmp)

# fastmm_test(<name> LABELS <labels...> LIBS <targets...>) globs tests/<dir>/*.cpp
function(fastmm_test name)
  cmake_parse_arguments(T "SERIAL" "DIR" "LABELS;LIBS;SOURCES" ${ARGN})
  if(NOT T_DIR)
    set(T_DIR ${name})
  endif()
  file(GLOB _src CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${T_DIR}/*.cpp)
  list(APPEND _src ${T_SOURCES})
  if(NOT _src)
    return()
  endif()
  add_executable(fastmm_${name}_tests ${_src})
  fastmm_target_defaults(fastmm_${name}_tests)
  target_link_libraries(fastmm_${name}_tests PRIVATE fastmm_test_support ${T_LIBS})
  # SERIAL: the test runs whole engines (engine, network and simulator threads) under timeouts;
  # sharing a small CI runner with other tests starves them, so ctest runs these one at a time.
  if(T_SERIAL)
    set(_serial RUN_SERIAL TRUE)
  else()
    set(_serial)
  endif()
  doctest_discover_tests(fastmm_${name}_tests TEST_PREFIX "${name}." PROPERTIES LABELS "${T_LABELS}" ${_serial})
endfunction()

fastmm_test(core      LABELS "unit;property" LIBS fastmm::core)
fastmm_test(strategies LABELS "unit"         LIBS fastmm::backtest)
fastmm_test(sim       LABELS "unit;property" LIBS fastmm::sim)
fastmm_test(backtest  LABELS "unit;replay"   LIBS fastmm::backtest)
fastmm_test(codecs    LABELS "unit;fixture"  LIBS fastmm::codecs fastmm::sim)
if(FASTMM_BUILD_NET)
  fastmm_test(net       LABELS "unit"           LIBS fastmm::net)
  fastmm_test(venues    LABELS "unit;fixture"   LIBS fastmm::venues)
  if(TARGET fastmm::sim_server)
    fastmm_test(integration SERIAL LABELS "integration" LIBS fastmm::venues fastmm::sim_server fastmm::backtest)
    fastmm_test(live        LABELS "live"        LIBS fastmm::venues fastmm::backtest)
  endif()
endif()

# Hot-path no-allocation harness: the ONLY binary that overrides global operator new. Not built
# under TSan: its runtime defines operator new/delete itself and the link fails on duplicates.
file(GLOB _hot CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/hotpath/*.cpp)
if(_hot AND NOT FASTMM_ENABLE_TSAN)
  add_executable(fastmm_hotpath_tests ${_hot} support/alloc_counter.cpp)
  fastmm_target_defaults(fastmm_hotpath_tests)
  target_link_libraries(fastmm_hotpath_tests PRIVATE fastmm_test_support fastmm::backtest fastmm::codecs)
  doctest_discover_tests(fastmm_hotpath_tests TEST_PREFIX "hotpath." PROPERTIES LABELS "noalloc")
endif()
# The reactor's dispatch path (hotpath/noalloc_reactor_test.cpp) needs the networking library.
if(TARGET fastmm_hotpath_tests AND TARGET fastmm::net)
  target_link_libraries(fastmm_hotpath_tests PRIVATE fastmm::net)
  target_compile_definitions(fastmm_hotpath_tests PRIVATE FASTMM_HOTPATH_NET=1)
endif()

# fastmm-top without a running engine: a clear message and exit code 3.
if(TARGET fastmm-top)
  add_test(NAME apps.fastmm-top.no_status COMMAND fastmm-top --once --path ${CMAKE_CURRENT_BINARY_DIR}/tmp/no-such.status)
  set_tests_properties(apps.fastmm-top.no_status PROPERTIES LABELS "unit" WILL_FAIL TRUE)
endif()


# tools/pnl_report.py checks its journal reader and PnL arithmetic on a synthetic journal.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME tools.pnl_report.self_test COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/pnl_report.py --self-test)
  set_tests_properties(tools.pnl_report.self_test PROPERTIES LABELS "unit")
endif()
