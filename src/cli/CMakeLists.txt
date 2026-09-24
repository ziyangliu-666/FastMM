# fastmm_cli11: CLI11 and command_line.hpp, the conventions the command lines share (help layout,
# --version, exit code 2 on a bad command line, durations). Internal: link it as
# $<BUILD_INTERFACE:fastmm_cli11> so the installed libraries do not depend on it.
add_library(fastmm_cli11 INTERFACE)
target_include_directories(fastmm_cli11 INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fastmm_cli11 INTERFACE CLI11::CLI11)
