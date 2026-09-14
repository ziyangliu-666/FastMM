# Runs one command-line invocation and checks its exit code and output. Invoked by ctest
# (tests/CMakeLists.txt):
#   cmake -DCMD=<exe@@arg@@...> -DEXPECT_RC=<n> [-DEXPECT_REGEX=<regex>] [-DREJECT_REGEX=<regex>]
#         [-DCOMPARE_CMD=<exe@@arg@@...>] [-DWORKDIR=<dir>] -P run_cli.cmake
# CMD and COMPARE_CMD separate their arguments with @@ (a ';' list would be split by ctest).
# COMPARE_CMD: its stdout must equal CMD's (both must exit with EXPECT_RC).
string(REPLACE "@@" ";" CMD "${CMD}")
string(REPLACE "@@" ";" COMPARE_CMD "${COMPARE_CMD}")
if(WORKDIR)
  file(MAKE_DIRECTORY ${WORKDIR})
else()
  set(WORKDIR ${CMAKE_CURRENT_BINARY_DIR})
endif()
execute_process(COMMAND ${CMD} WORKING_DIRECTORY ${WORKDIR}
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
message("$ ${CMD}\n-- exit ${rc}\n-- stdout:\n${out}\n-- stderr:\n${err}")
if(NOT "${rc}" STREQUAL "${EXPECT_RC}")
  message(FATAL_ERROR "run_cli: exit code ${rc}, expected ${EXPECT_RC}")
endif()
if(DEFINED EXPECT_REGEX AND NOT "${out}${err}" MATCHES "${EXPECT_REGEX}")
  message(FATAL_ERROR "run_cli: output does not match ${EXPECT_REGEX}")
endif()
if(DEFINED REJECT_REGEX AND "${out}${err}" MATCHES "${REJECT_REGEX}")
  message(FATAL_ERROR "run_cli: output matches ${REJECT_REGEX}")
endif()
if(COMPARE_CMD)
  execute_process(COMMAND ${COMPARE_CMD} WORKING_DIRECTORY ${WORKDIR}
    RESULT_VARIABLE rc2 OUTPUT_VARIABLE out2 ERROR_VARIABLE err2 TIMEOUT 120)
  message("$ ${COMPARE_CMD}\n-- exit ${rc2}\n-- stdout:\n${out2}\n-- stderr:\n${err2}")
  if(NOT "${rc2}" STREQUAL "${EXPECT_RC}")
    message(FATAL_ERROR "run_cli: exit code ${rc2}, expected ${EXPECT_RC}")
  endif()
  if(NOT "${out}" STREQUAL "${out2}")
    message(FATAL_ERROR "run_cli: the two commands print different output")
  endif()
endif()
