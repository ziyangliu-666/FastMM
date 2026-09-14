# Builds one compile_fail target and checks its outcome. Invoked by ctest (tests/CMakeLists.txt):
#   cmake -DBUILD_DIR=... -DTARGET=... -DOBJECTS=... -DEXPECT=fail|pass|any -P run_case.cmake
# The object file is removed first, so the compiler runs (and prints its diagnostics) every time.
# The test's PASS_REGULAR_EXPRESSION matches the expected fastmm diagnostic; an unexpected outcome
# prints "compile_fail: unexpected", which its FAIL_REGULAR_EXPRESSION rejects.
file(REMOVE ${OBJECTS})
execute_process(
  COMMAND ${CMAKE_COMMAND} --build ${BUILD_DIR} --target ${TARGET}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)
message("${out}${err}")
if(EXPECT STREQUAL "fail" AND rc EQUAL 0)
  message("compile_fail: unexpected success building ${TARGET}")
elseif(EXPECT STREQUAL "pass" AND NOT rc EQUAL 0)
  message(FATAL_ERROR "compile_fail: unexpected failure building ${TARGET}")
endif()
