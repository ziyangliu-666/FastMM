# One test executable per library; doctest subcases inside. Labels drive ctest presets.

add_library(fastmm_test_support STATIC support/doctest_main.cpp)
target_link_libraries(fastmm_test_support PUBLIC doctest::doctest fastmm::core)
target_include_directories(fastmm_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_compile_definitions(fastmm_test_support PUBLIC
  FASTMM_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  FASTMM_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tmp)

# fastmm_test(<name> LABELS <labels...> LIBS <targets...> [EXCLUDE_REGEX <regex>]) globs
# tests/<dir>/*.cpp, minus the paths matching EXCLUDE_REGEX (tests of an optional library).
function(fastmm_test name)
  cmake_parse_arguments(T "SERIAL" "DIR;EXCLUDE_REGEX" "LABELS;LIBS;SOURCES" ${ARGN})
  if(NOT T_DIR)
    set(T_DIR ${name})
  endif()
  file(GLOB _src CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${T_DIR}/*.cpp)
  if(T_EXCLUDE_REGEX)
    list(FILTER _src EXCLUDE REGEX "${T_EXCLUDE_REGEX}")
  endif()
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

# fastmm_relabel_tests(<target> <name regex> <labels>): the discovered tests of <target> whose ctest
# name matches get <labels> instead of the binary's labels. Test names are only known after the
# build (doctest_discover_tests), so this runs as a ctest include file after the discovered list.
function(fastmm_relabel_tests target regex labels)
  set(_file "${CMAKE_CURRENT_BINARY_DIR}/${target}_relabel.cmake")
  file(WRITE "${_file}"
    "foreach(_t IN LISTS ${target}_TESTS)\n"
    "  if(_t MATCHES [==[${regex}]==])\n"
    "    set_tests_properties(\"\${_t}\" PROPERTIES LABELS [==[${labels}]==])\n"
    "  endif()\n"
    "endforeach()\n")
  set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${_file}")
endfunction()

fastmm_test(core      LABELS "unit;property" LIBS fastmm::core)
# The store tests reach into src/store/sqlite_schema.hpp for the migration steps, so they link
# SQLite directly; fastmm::store keeps it private.
fastmm_test(store     LABELS "unit"          LIBS fastmm::store sqlite3)
fastmm_test(strategies LABELS "unit"         LIBS fastmm::backtest)
# A strategy library in its own STATIC archive, referenced only through its registration functions.
add_library(fastmm_test_strategy_module STATIC
  modules/module/strategies.cpp modules/module/shadow_register.cpp modules/module/shadow_sim.cpp)
fastmm_target_defaults(fastmm_test_strategy_module)
target_include_directories(fastmm_test_strategy_module PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/modules)
target_link_libraries(fastmm_test_strategy_module PUBLIC fastmm::sim)
fastmm_test(modules   LABELS "unit"          LIBS fastmm_test_strategy_module fastmm::backtest)
fastmm_test(sim       LABELS "unit;property" LIBS fastmm::sim)
fastmm_test(backtest  LABELS "unit;replay"   LIBS fastmm::backtest)
# The FIX and MDP3 tests (here and in hotpath/) follow their codec's option: without it the
# library has no such code to link against.
set(_codec_skip)
if(NOT FASTMM_CODEC_FIX)
  list(APPEND _codec_skip "/fix_" "/codecs_fix_")
endif()
if(NOT FASTMM_CODEC_MDP3)
  list(APPEND _codec_skip "/mdp3_" "/sbe_schema_" "/codecs_mdp3_")
endif()
list(JOIN _codec_skip "|" _codec_skip_regex)
fastmm_test(codecs    LABELS "unit;fixture"  LIBS fastmm::codecs fastmm::sim_itch_publisher
                      EXCLUDE_REGEX "${_codec_skip_regex}")
# Shipped configs/*.toml: load, schema and [strategy.params] through the registry (label config).
fastmm_test(config    LABELS "config"        LIBS fastmm::backtest)
if(TARGET fastmm_config_tests)
  target_compile_definitions(fastmm_config_tests PRIVATE FASTMM_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs")
  # The tutorial's configs run its strategy (examples/cpp/tutorial).
  if(TARGET tutorial_strategies)
    target_link_libraries(fastmm_config_tests PRIVATE tutorial_strategies)
    target_compile_definitions(fastmm_config_tests PRIVATE FASTMM_TUTORIAL_STRATEGIES=1)
  endif()
endif()

# Docs checks (label docs, docs/reference/strategy-api.md and docs/reference/public-api.md):
#   docs.strategy_api.*   tests/docs/strategy_api_doc_test.cpp, the source of the strategy API
#                         reference's code blocks
#   docs.public_headers   every header of docs/api/public-headers.txt compiles on its own
fastmm_test(docs LABELS "unit;docs" LIBS fastmm::sim)
if(TARGET fastmm_docs_tests)
  fastmm_relabel_tests(fastmm_docs_tests "^docs\\." "unit;docs")  # discovered tests keep one label
endif()
set(_manifest ${PROJECT_SOURCE_DIR}/docs/api/public-headers.txt)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_manifest})
file(STRINGS ${_manifest} _manifest_lines REGEX "^[^#]")
set(_header_tus)
foreach(_line IN LISTS _manifest_lines)
  if(NOT _line MATCHES "^(tier1|tier2) (net )?([A-Za-z0-9_/.]+\\.hpp)$")
    message(FATAL_ERROR "docs/api/public-headers.txt: malformed line '${_line}'")
  endif()
  set(_header ${CMAKE_MATCH_3})
  if(CMAKE_MATCH_2 AND NOT FASTMM_BUILD_NET)
    continue()
  endif()
  if(NOT EXISTS ${PROJECT_SOURCE_DIR}/include/${_header})
    message(FATAL_ERROR "docs/api/public-headers.txt: include/${_header} does not exist")
  endif()
  string(MAKE_C_IDENTIFIER ${_header} _tu)
  set(_tu ${CMAKE_CURRENT_BINARY_DIR}/public_headers/${_tu}.cpp)
  file(CONFIGURE OUTPUT ${_tu} CONTENT "#include \"${_header}\"  // alone: nothing before it\n")
  list(APPEND _header_tus ${_tu})
endforeach()
if(NOT FASTMM_ENABLE_ASAN AND NOT FASTMM_ENABLE_TSAN)
  add_library(fastmm_public_headers OBJECT EXCLUDE_FROM_ALL ${_header_tus})
  fastmm_target_defaults(fastmm_public_headers)
  target_link_libraries(fastmm_public_headers PRIVATE fastmm::backtest fastmm::codecs)
  if(TARGET fastmm::live)
    target_link_libraries(fastmm_public_headers PRIVATE fastmm::live fastmm::venues fastmm::net)
  endif()
  add_test(NAME docs.public_headers
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target fastmm_public_headers)
  set_tests_properties(docs.public_headers PROPERTIES
    LABELS "docs" RESOURCE_LOCK fastmm_compile_fail TIMEOUT 3600)
endif()
if(FASTMM_BUILD_NET)
  fastmm_test(net       LABELS "unit"           LIBS fastmm::net)
  # AF_XDP backend. The privileged cases (verifier load, BPF_PROG_TEST_RUN, veth receive) skip
  # without root or the capabilities; `sudo scripts/xdp-test.sh` runs them.
  fastmm_test(xdp       LABELS "unit"           LIBS fastmm::net)
  # DPDK backend (-DFASTMM_WITH_DPDK=ON only; no CI job enables it): EAL without hugepages and the
  # net_af_packet vdev on a veth in a user namespace, so it runs unprivileged.
  if(TARGET fastmm_dpdk)
    fastmm_test(dpdk    LABELS "dpdk"           LIBS fastmm::net)
  endif()
  fastmm_test(venues    LABELS "unit;fixture"   LIBS fastmm::venues)
  # Opt-in testnet checks (tests/venues/live_*_test.cpp): `ctest -L live` selects them, the test
  # presets exclude the label, and without FASTMM_LIVE_TESTS=1 they skip.
  if(TARGET fastmm_venues_tests)
    fastmm_relabel_tests(fastmm_venues_tests "^venues\\.live\\." "live")
  endif()
  if(TARGET fastmm::sim_server)
    fastmm_test(integration SERIAL LABELS "integration" LIBS fastmm::venues fastmm::sim_server fastmm::sim_itch fastmm::backtest fastmm::live)
    fastmm_test(live        LABELS "live"        LIBS fastmm::venues fastmm::backtest)
  endif()
endif()

# Hot-path no-allocation harness: the ONLY binary that overrides global operator new. Not built
# under TSan: its runtime defines operator new/delete itself and the link fails on duplicates.
file(GLOB _hot CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/hotpath/*.cpp)
if(_codec_skip_regex)
  list(FILTER _hot EXCLUDE REGEX "${_codec_skip_regex}")
endif()
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
# Venue parsers and encoders (hotpath/venues_noalloc_test.cpp) need the venues library.
if(TARGET fastmm_hotpath_tests AND TARGET fastmm::venues)
  target_link_libraries(fastmm_hotpath_tests PRIVATE fastmm::venues)
  target_compile_definitions(fastmm_hotpath_tests PRIVATE FASTMM_HOTPATH_VENUES=1)
endif()

# Compile-fail tests (label compile_fail): each case must fail to build with the expected fastmm
# diagnostic, and the same source with FASTMM_CF_CONTROL must build, so a broken include cannot pass
# as an expected failure. The targets are EXCLUDE_FROM_ALL; ctest builds them one at a time.
#   fastmm_compile_fail(<case> EXPECT fail|any REGEX <regex> [SOURCE <file stem>] [OPTIONS <flags>])
# A ';' inside REGEX must be written as [;] (CMake splits lists on it).
function(fastmm_compile_fail name)
  cmake_parse_arguments(CF "" "EXPECT;REGEX;SOURCE" "OPTIONS" ${ARGN})
  if(NOT CF_SOURCE)
    set(CF_SOURCE ${name})
  endif()
  foreach(_variant case control)
    set(_tgt fastmm_cf_${name}_${_variant})
    add_library(${_tgt} OBJECT EXCLUDE_FROM_ALL ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/${CF_SOURCE}.cpp)
    fastmm_target_defaults(${_tgt})
    target_link_libraries(${_tgt} PRIVATE fastmm::core)
    target_compile_options(${_tgt} PRIVATE ${CF_OPTIONS})
  endforeach()
  target_compile_definitions(fastmm_cf_${name}_control PRIVATE FASTMM_CF_CONTROL=1)
  set(_expect ${CF_EXPECT})
  foreach(_variant case control)
    set(_tgt fastmm_cf_${name}_${_variant})
    set(_test compile_fail.${name})
    if(_variant STREQUAL "control")
      set(_expect pass)
      set(_test compile_fail.${name}.control)
    endif()
    add_test(NAME ${_test}
      COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR} -DTARGET=${_tgt}
              "-DOBJECTS=$<TARGET_OBJECTS:${_tgt}>" -DEXPECT=${_expect}
              -P ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/run_case.cmake)
    set_tests_properties(${_test} PROPERTIES LABELS compile_fail RESOURCE_LOCK fastmm_compile_fail)
  endforeach()
  set_tests_properties(compile_fail.${name} PROPERTIES
    PASS_REGULAR_EXPRESSION "${CF_REGEX}" FAIL_REGULAR_EXPRESSION "compile_fail: unexpected")
endfunction()

set(_cf_prefix "fastmm: ")
fastmm_compile_fail(wrong_trade_signature EXPECT fail REGEX
  "${_cf_prefix}on_trade has the wrong signature or is not public[;] expected void on_trade\\(auto& ctx, InstrumentId id, const TradeMsg& m\\)")
fastmm_compile_fail(swapped_fill_arguments EXPECT fail REGEX
  "${_cf_prefix}on_fill has the wrong signature or is not public[;] expected void on_fill\\(auto& ctx, const Fill& fill\\)")
fastmm_compile_fail(private_hook EXPECT fail REGEX
  "${_cf_prefix}on_book has the wrong signature or is not public[;] expected void on_book\\(auto& ctx, InstrumentId id, const auto& book\\)")
fastmm_compile_fail(near_miss_name EXPECT any REGEX
  "${_cf_prefix}on_fills is not a hook[;] did you mean on_fill\\?")
fastmm_compile_fail(near_miss_name_werror SOURCE near_miss_name OPTIONS -Werror=deprecated-declarations
  EXPECT fail REGEX "${_cf_prefix}on_fills is not a hook[;] did you mean on_fill\\?")
fastmm_compile_fail(non_void_return EXPECT fail REGEX
  "${_cf_prefix}on_timer has the wrong signature or is not public")
fastmm_compile_fail(ambiguous_bases EXPECT fail REGEX
  "${_cf_prefix}on_quoting has the wrong signature or is not public")
fastmm_compile_fail(data_member_hook_name EXPECT fail REGEX
  "${_cf_prefix}on_connection has the wrong signature or is not public")
fastmm_compile_fail(literal_qty_nine_decimals EXPECT fail REGEX
  "${_cf_prefix}_qty literal needs more than 8 decimals")
fastmm_compile_fail(literal_bps_five_decimals EXPECT fail REGEX
  "${_cf_prefix}_bps literal needs more than 4 decimals")

# Link-fail tests (label compile_fail): the case compiles but must fail to link with the expected
# message (a missing factory instantiation); the control (FASTMM_CF_CONTROL) must link.
#   fastmm_link_fail(<case> REGEX <regex>)
function(fastmm_link_fail name)
  cmake_parse_arguments(LF "" "REGEX" "" ${ARGN})
  foreach(_variant case control)
    set(_tgt fastmm_lf_${name}_${_variant})
    add_executable(${_tgt} EXCLUDE_FROM_ALL ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/${name}.cpp)
    fastmm_target_defaults(${_tgt})
    target_link_libraries(${_tgt} PRIVATE fastmm::strategies)
  endforeach()
  target_compile_definitions(fastmm_lf_${name}_control PRIVATE FASTMM_CF_CONTROL=1)
  foreach(_variant case control)
    set(_tgt fastmm_lf_${name}_${_variant})
    if(_variant STREQUAL "control")
      set(_expect pass)
      set(_test link_fail.${name}.control)
    else()
      set(_expect fail)
      set(_test link_fail.${name})
    endif()
    add_test(NAME ${_test}
      COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR} -DTARGET=${_tgt}
              "-DOBJECTS=$<TARGET_FILE:${_tgt}>" -DEXPECT=${_expect}
              -P ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/run_case.cmake)
    set_tests_properties(${_test} PROPERTIES LABELS compile_fail RESOURCE_LOCK fastmm_compile_fail)
  endforeach()
  set_tests_properties(link_fail.${name} PROPERTIES
    PASS_REGULAR_EXPRESSION "${LF_REGEX}" FAIL_REGULAR_EXPRESSION "compile_fail: unexpected")
endfunction()

fastmm_link_fail(missing_factory_instantiation REGEX
  "undefined (reference|symbol).*fastmm::live_factory<LinkMM>")

# fastmm-top without a running engine: a clear message and exit code 3.
if(TARGET fastmm-top)
  add_test(NAME apps.fastmm-top.no_status COMMAND fastmm-top --once --path ${CMAKE_CURRENT_BINARY_DIR}/tmp/no-such.status)
  set_tests_properties(apps.fastmm-top.no_status PROPERTIES LABELS "unit" WILL_FAIL TRUE)
endif()


# Command-line entry points (label cli): exit codes and messages of fastmm::cli::live / backtest,
# --list-strategies parity, and strategy modules passed to the entry points.
#   fastmm_cli_test(<name> EXPECT_RC <n> CMD <exe> <args...> [COMPARE_CMD <exe> <args...>]
#                   [EXPECT_REGEX <regex>] [REJECT_REGEX <regex>])
function(fastmm_cli_test name)
  cmake_parse_arguments(C "" "EXPECT_RC;EXPECT_REGEX;REJECT_REGEX" "CMD;COMPARE_CMD" ${ARGN})
  # Command lists are joined with @@: ctest would split a ';' list into separate arguments.
  string(JOIN "@@" _cmd ${C_CMD})
  set(_args "-DCMD=${_cmd}" -DEXPECT_RC=${C_EXPECT_RC} -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/tmp/cli)
  if(DEFINED C_EXPECT_REGEX)
    list(APPEND _args "-DEXPECT_REGEX=${C_EXPECT_REGEX}")
  endif()
  if(DEFINED C_REJECT_REGEX)
    list(APPEND _args "-DREJECT_REGEX=${C_REJECT_REGEX}")
  endif()
  if(C_COMPARE_CMD)
    string(JOIN "@@" _compare ${C_COMPARE_CMD})
    list(APPEND _args "-DCOMPARE_CMD=${_compare}")
  endif()
  add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} ${_args} -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/run_cli.cmake)
  set_tests_properties(${name} PROPERTIES LABELS "unit;cli")
endfunction()

set(_json_builtin ".name.: .options_mm., .transports.: \\[.sim., .replay., .live.\\]")
if(TARGET fastmm-backtest)
  fastmm_cli_test(apps.fastmm-backtest.list_strategies_json EXPECT_RC 0
    CMD $<TARGET_FILE:fastmm-backtest> --list-strategies --format json EXPECT_REGEX "${_json_builtin}")
  fastmm_cli_test(apps.fastmm-backtest.format_without_list EXPECT_RC 2
    CMD $<TARGET_FILE:fastmm-backtest> --format json EXPECT_REGEX "fastmm-backtest: --format applies to --list-strategies")
  fastmm_cli_test(apps.fastmm-backtest.unknown_strategy EXPECT_RC 3
    CMD $<TARGET_FILE:fastmm-backtest> --config ${PROJECT_SOURCE_DIR}/configs/backtest-example.toml --strategy nope
    EXPECT_REGEX "fastmm-backtest: unknown strategy 'nope'")
  # configs/backtest-example.toml with a misspelt [risk] and [backtest] key: both programs warn with
  # the key and its line, then run.
  file(READ ${PROJECT_SOURCE_DIR}/configs/backtest-example.toml _bt_typos)
  string(REPLACE "[risk]\n" "[risk]\nmax_postion = \"0.05\"\n" _bt_typos "${_bt_typos}")
  string(REPLACE "[backtest]\n" "[backtest]\nduraton_s = 5\n" _bt_typos "${_bt_typos}")
  set(_bt_typos_file ${CMAKE_CURRENT_BINARY_DIR}/tmp/cli/backtest-typos.toml)
  file(CONFIGURE OUTPUT ${_bt_typos_file} CONTENT "${_bt_typos}" @ONLY)
  set(_bt_typos_regex "warning: .*backtest-typos.toml: unknown key 'risk.max_postion' ignored \\(line [0-9]+\\).*backtest-typos.toml: unknown key 'backtest.duraton_s' ignored \\(line [0-9]+\\)")
  fastmm_cli_test(apps.fastmm-backtest.unknown_config_keys EXPECT_RC 0
    CMD $<TARGET_FILE:fastmm-backtest> --config ${_bt_typos_file} --data synthetic --duration 1 --out -
    EXPECT_REGEX "${_bt_typos_regex}")
  if(TARGET fastmm-replay)
    fastmm_cli_test(apps.fastmm-replay.unknown_config_keys EXPECT_RC 0
      CMD $<TARGET_FILE:fastmm-replay> --journal ${PROJECT_SOURCE_DIR}/tests/fixtures/journals/sample_1000.fmj --config ${_bt_typos_file}
      EXPECT_REGEX "${_bt_typos_regex}")
  endif()
endif()
if(TARGET fastmm-pnl)
  fastmm_cli_test(apps.fastmm-pnl.no_command EXPECT_RC 2
    CMD $<TARGET_FILE:fastmm-pnl> --engine mm
    EXPECT_REGEX "a command is required")
  fastmm_cli_test(apps.fastmm-pnl.unknown_backend EXPECT_RC 2
    CMD $<TARGET_FILE:fastmm-pnl> sessions --backend clickhouse
    EXPECT_REGEX "no queryable backend 'clickhouse' \\(have none, sqlite\\)")
  fastmm_cli_test(apps.fastmm-pnl.missing_store EXPECT_RC 2
    CMD $<TARGET_FILE:fastmm-pnl> sessions --store ${CMAKE_CURRENT_BINARY_DIR}/tmp/cli/no-such.db
    EXPECT_REGEX "fastmm-pnl: .*no-such.db")
endif()

if(TARGET fastmm-live)
  # Built-in parity: the live and backtest apps offer the same strategies, for all three transports.
  fastmm_cli_test(apps.list_strategies.parity_text EXPECT_RC 0
    CMD $<TARGET_FILE:fastmm-live> --list-strategies COMPARE_CMD $<TARGET_FILE:fastmm-backtest> --list-strategies
    EXPECT_REGEX "avellaneda_stoikov")
  fastmm_cli_test(apps.list_strategies.parity_json EXPECT_RC 0
    CMD $<TARGET_FILE:fastmm-live> --list-strategies --format json
    COMPARE_CMD $<TARGET_FILE:fastmm-backtest> --list-strategies --format json
    EXPECT_REGEX "${_json_builtin}")
  fastmm_cli_test(apps.fastmm-live.bad_format EXPECT_RC 2
    CMD $<TARGET_FILE:fastmm-live> --list-strategies --format yaml EXPECT_REGEX "fastmm-live: bad --format 'yaml'")
  fastmm_cli_test(apps.fastmm-live.bad_param_syntax EXPECT_RC 2
    CMD $<TARGET_FILE:fastmm-live> --config x.toml --param half_spread_bps EXPECT_REGEX "--param expects key=value")
  # configs/sim-local.toml with its venue on a closed port: every case below fails before trading.
  file(READ ${PROJECT_SOURCE_DIR}/configs/sim-local.toml _sim_local)
  string(REPLACE "127.0.0.1:9080" "127.0.0.1:1" _sim_local "${_sim_local}")
  set(_unreachable ${CMAKE_CURRENT_BINARY_DIR}/tmp/cli/sim-local-unreachable.toml)
  file(CONFIGURE OUTPUT ${_unreachable} CONTENT "${_sim_local}" @ONLY)
  set(_live_offline $<TARGET_FILE:fastmm-live> --config ${_unreachable} --dry-run --no-journal --no-status --duration 1s)
  fastmm_cli_test(apps.fastmm-live.unknown_strategy EXPECT_RC 3
    CMD ${_live_offline} --strategy nope
    EXPECT_REGEX "fastmm-live: unknown strategy 'nope' \\(available: basic_mm avellaneda_stoikov options_mm\\)")
  fastmm_cli_test(apps.fastmm-live.unknown_param EXPECT_RC 3
    CMD ${_live_offline} --param no_such_param=1
    EXPECT_REGEX "fastmm-live: basic_mm: unknown parameter 'no_such_param'")
  # --strategy clears the config's basic_mm parameters: the run gets past the parameter check and
  # stops at the unreachable venue (exit 4), and basic_mm parameters no longer apply.
  fastmm_cli_test(apps.fastmm-live.strategy_clears_params EXPECT_RC 4
    CMD ${_live_offline} --strategy avellaneda_stoikov --param gamma=0.2
    EXPECT_REGEX "note: ignoring \\[strategy.params\\] of 'basic_mm' for --strategy avellaneda_stoikov"
    REJECT_REGEX "unknown parameter")
  fastmm_cli_test(apps.fastmm-live.params_follow_strategy EXPECT_RC 3
    CMD ${_live_offline} --strategy avellaneda_stoikov --param half_spread_bps=5
    EXPECT_REGEX "fastmm-live: avellaneda_stoikov: unknown parameter 'half_spread_bps'")
  string(REPLACE "[engine]\n" "[engine]\non_kill = \"restart\"\n" _bad_on_kill "${_sim_local}")
  set(_bad_on_kill_file ${CMAKE_CURRENT_BINARY_DIR}/tmp/cli/sim-local-bad-on-kill.toml)
  file(CONFIGURE OUTPUT ${_bad_on_kill_file} CONTENT "${_bad_on_kill}" @ONLY)
  fastmm_cli_test(apps.fastmm-live.bad_on_kill EXPECT_RC 3
    CMD $<TARGET_FILE:fastmm-live> --config ${_bad_on_kill_file} --dry-run --no-journal --no-status
    EXPECT_REGEX "on_kill must be exit\\|stay")
  fastmm_cli_test(apps.fastmm-live.help_lists_exit_codes EXPECT_RC 0
    CMD $<TARGET_FILE:fastmm-live> --help
    EXPECT_REGEX "Exit codes:.*  6  kill switch tripped by the engine \\(on_kill = \"exit\"\\)")
endif()

if(TARGET fastmm-sim-itch)
  fastmm_cli_test(apps.fastmm-sim-itch.bad_drop_rate EXPECT_RC 2
    CMD $<TARGET_FILE:fastmm-sim-itch> --drop-a 1.5 EXPECT_REGEX "fastmm-sim-itch: bad --drop-a '1.5'")
  fastmm_cli_test(apps.fastmm-sim-itch.bad_config EXPECT_RC 3
    CMD $<TARGET_FILE:fastmm-sim-itch> --line-a off --line-b off
    EXPECT_REGEX "sim-itch config: no multicast line")
endif()

# fastmm-live against fastmm-sim-itch as processes in two network namespaces joined by a veth pair
# (scripts/bench-e2e.sh, kernel backend, unpinned, 5 s), with [engine] threading = "split" and
# "single": the feed goes live, orders are sent and fastmm-replay --verify matches the journal.
# Exit code 77 (no user namespaces here) skips them.
if(TARGET fastmm-sim-itch AND TARGET fastmm-live AND TARGET fastmm-top AND TARGET fastmm-replay)
  foreach(_threading split single)
    set(_name integration.nasdaq_itch_processes)
    if(_threading STREQUAL "single")
      set(_name ${_name}_single)
    endif()
    add_test(NAME ${_name}
      COMMAND ${PROJECT_SOURCE_DIR}/scripts/bench-e2e.sh --duration 5 --spin adaptive --speed 4
              --threading ${_threading} --replay
              --sim-cpu -1 --engine-cpu -1 --net-cpu -1 --build ${PROJECT_BINARY_DIR}
              --out ${CMAKE_CURRENT_BINARY_DIR}/tmp/bench-e2e-${_threading})
    set_tests_properties(${_name} PROPERTIES
      LABELS "integration" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 180)
  endforeach()
  # The same with the OUCH connection on the user-space TCP (order_transport = "user_tcp").
  add_test(NAME integration.nasdaq_itch_processes_user_tcp
    COMMAND ${PROJECT_SOURCE_DIR}/scripts/bench-e2e.sh --duration 5 --spin adaptive --speed 4
            --order-transport user_tcp --sim-cpu -1 --engine-cpu -1 --net-cpu -1
            --build ${PROJECT_BINARY_DIR} --out ${CMAKE_CURRENT_BINARY_DIR}/tmp/bench-e2e-user-tcp)
  set_tests_properties(integration.nasdaq_itch_processes_user_tcp PROPERTIES
    LABELS "integration" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 180)
  # Unicast market data (the simulator sends to fastmm-live's address; no multicast).
  add_test(NAME integration.nasdaq_itch_processes_unicast
    COMMAND ${PROJECT_SOURCE_DIR}/scripts/bench-e2e.sh --duration 5 --spin adaptive --speed 4
            --md unicast --sim-cpu -1 --engine-cpu -1 --net-cpu -1
            --build ${PROJECT_BINARY_DIR} --out ${CMAKE_CURRENT_BINARY_DIR}/tmp/bench-e2e-unicast)
  set_tests_properties(integration.nasdaq_itch_processes_unicast PROPERTIES
    LABELS "integration" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 180)
  # DPDK as on a NIC bound to vfio-pci: the kernel only behind the exception tap, unicast lines, and
  # UserTcp on the DPDK port with fastmm-live's own address and a fixed port.
  if(TARGET fastmm_dpdk)
    add_test(NAME dpdk.nasdaq_itch_processes_exception_user_tcp
      COMMAND ${PROJECT_SOURCE_DIR}/scripts/bench-e2e.sh --duration 5 --speed 4 --backend dpdk
              --dpdk-exception --md unicast --order-transport user_tcp --user-tcp-ip 10.211.0.2
              --user-tcp-port 61001 --sim-cpu -1 --engine-cpu -1 --net-cpu -1
              --build ${PROJECT_BINARY_DIR} --out ${CMAKE_CURRENT_BINARY_DIR}/tmp/bench-e2e-dpdk)
    set_tests_properties(dpdk.nasdaq_itch_processes_exception_user_tcp PROPERTIES
      LABELS "dpdk" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 180)
  endif()
endif()

# Apps built from the test strategy library: the module is listed for every transport, runs, and a
# name clash with a built-in strategy exits with code 3.
foreach(_app backtest conflict)
  add_executable(fastmm_test_module_${_app} modules/apps/${_app}_main.cpp)
  fastmm_target_defaults(fastmm_test_module_${_app})
  target_link_libraries(fastmm_test_module_${_app} PRIVATE fastmm_test_strategy_module fastmm::backtest)
endforeach()
set(_json_module ".name.: .test_module_mm., .transports.: \\[.sim., .replay., .live.\\]")
fastmm_cli_test(modules.cli.backtest_lists_module EXPECT_RC 0
  CMD $<TARGET_FILE:fastmm_test_module_backtest> --list-strategies --format json EXPECT_REGEX "${_json_module}")
fastmm_cli_test(modules.cli.backtest_runs_module EXPECT_RC 0
  CMD $<TARGET_FILE:fastmm_test_module_backtest> --config ${PROJECT_SOURCE_DIR}/configs/backtest-example.toml
      --data synthetic --strategy test_module_mm --param edge_ticks=1 --duration 10 --out -
  EXPECT_REGEX "fills \\(maker / taker\\)")
fastmm_cli_test(modules.cli.conflict_exits_3 EXPECT_RC 3
  CMD $<TARGET_FILE:fastmm_test_module_conflict> --list-strategies
  EXPECT_REGEX "fastmm_test_module_conflict: strategy 'basic_mm' \\(sim\\) is already registered by different code")
if(TARGET fastmm::live)
  add_executable(fastmm_test_module_live modules/apps/live_main.cpp)
  fastmm_target_defaults(fastmm_test_module_live)
  target_link_libraries(fastmm_test_module_live PRIVATE fastmm_test_strategy_module fastmm::live)
  fastmm_cli_test(modules.cli.live_lists_module EXPECT_RC 0
    CMD $<TARGET_FILE:fastmm_test_module_live> --list-strategies --format json EXPECT_REGEX "${_json_module}")
endif()

# The package config of a FastMM built without networking: find_package(fastmm COMPONENTS live)
# fails and says why. Only fastmmConfig.cmake is generated; the check returns before any target
# file is read.
include(CMakePackageConfigHelpers)
set(_nonet_config_dir ${CMAKE_CURRENT_BINARY_DIR}/tmp/fastmm-config-nonet)
block(SCOPE_FOR VARIABLES)
  set(FASTMM_BUILD_NET OFF)
  configure_package_config_file(${PROJECT_SOURCE_DIR}/cmake/fastmmConfig.cmake.in
    ${_nonet_config_dir}/fastmmConfig.cmake INSTALL_DESTINATION lib/cmake/fastmm)
endblock()
add_test(NAME cmake.find_package.live_without_net
  COMMAND ${CMAKE_COMMAND} -S ${CMAKE_CURRENT_SOURCE_DIR}/cmake/find_live_without_net
          -B ${CMAKE_CURRENT_BINARY_DIR}/tmp/find-live-without-net -Dfastmm_DIR=${_nonet_config_dir})
set_tests_properties(cmake.find_package.live_without_net PROPERTIES LABELS "unit"
  PASS_REGULAR_EXPRESSION "component 'live' \\(fastmm::live\\) is not available:.*FASTMM_BUILD_NET=OFF\\.")

# tools/pnl_report.py checks its journal reader and PnL arithmetic on a synthetic journal.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME tools.pnl_report.self_test COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/pnl_report.py --self-test)
  set_tests_properties(tools.pnl_report.self_test PROPERTIES LABELS "unit")
endif()

# fastmm-live --help and docs/reference/cli.md (the one exit-code table) list the same codes.
if(Python3_Interpreter_FOUND AND TARGET fastmm-live)
  add_test(NAME apps.fastmm-live.exit_codes_documented
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/support/exit_codes_doc.py
            $<TARGET_FILE:fastmm-live>
            ${PROJECT_SOURCE_DIR}/docs/reference/cli.md)
  set_tests_properties(apps.fastmm-live.exit_codes_documented PROPERTIES LABELS "unit;cli")
endif()

# fastmm-top --once messages: missing file, unpublished segment, segment of another version.
if(Python3_Interpreter_FOUND AND TARGET fastmm-top)
  add_test(NAME apps.fastmm-top.messages
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/support/fastmm_top_messages.py
            $<TARGET_FILE:fastmm-top> ${PROJECT_SOURCE_DIR}/include/fastmm/core/status_segment.hpp
            ${CMAKE_CURRENT_BINARY_DIR}/tmp)
  set_tests_properties(apps.fastmm-top.messages PROPERTIES LABELS "unit")
endif()
