# Two INTERFACE targets every fastmm target links PRIVATE-ly:
#   fastmm_warnings   - strict warnings (-Werror when FASTMM_WERROR)
#   fastmm_lowlatency - codegen flags for the hot path
add_library(fastmm_warnings INTERFACE)
add_library(fastmm_lowlatency INTERFACE)
add_library(fastmm::warnings ALIAS fastmm_warnings)
add_library(fastmm::lowlatency ALIAS fastmm_lowlatency)
set_target_properties(fastmm_warnings PROPERTIES EXPORT_NAME warnings)
set_target_properties(fastmm_lowlatency PROPERTIES EXPORT_NAME lowlatency)

target_compile_options(fastmm_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Woverloaded-virtual
  -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
  -Wno-unused-function)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(fastmm_warnings INTERFACE -Wduplicated-cond -Wlogical-op -Wuseless-cast)
endif()
if(FASTMM_WERROR)
  target_compile_options(fastmm_warnings INTERFACE -Werror)
endif()

# Frame pointers for perf/flamegraphs are near-free on x86-64; direct calls via -fno-plt.
target_compile_options(fastmm_lowlatency INTERFACE
  -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -fno-plt -fno-semantic-interposition)
# Code alignment. With gcc's defaults (16 B), edits to code the benchmark never runs moved
# BM_EngineStep_Sim by up to 8 %; with these, by 2 %, and the engine benchmarks got faster
# (bench/README.md, "Code alignment"). About 4 % more .text. gcc only: clang warns that it
# ignores -falign-jumps, and it was not measured with clang.
if(FASTMM_ALIGN_CODE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(fastmm_lowlatency INTERFACE
    -falign-functions=64 -falign-loops=32 -falign-jumps=32)
endif()
if(FASTMM_NATIVE_ARCH)
  target_compile_options(fastmm_lowlatency INTERFACE -march=native)
else()
  # Portable baseline with SSE4.2 (crc32) + POPCNT; x86-64-v2 is 2009+ hardware.
  target_compile_options(fastmm_lowlatency INTERFACE -march=x86-64-v2)
endif()

if(FASTMM_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _fastmm_ipo OUTPUT _fastmm_ipo_msg LANGUAGES CXX)
  if(_fastmm_ipo)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  else()
    message(STATUS "fastmm: LTO not supported: ${_fastmm_ipo_msg}")
  endif()
endif()

if(FASTMM_USE_CCACHE)
  find_program(FASTMM_CCACHE ccache)
  if(FASTMM_CCACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${FASTMM_CCACHE})
  endif()
endif()

# Helper: apply the standard property set to a fastmm target.
function(fastmm_target_defaults tgt)
  target_compile_features(${tgt} PUBLIC cxx_std_20)
  set_target_properties(${tgt} PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ${FASTMM_PIC})
  target_link_libraries(${tgt} PRIVATE fastmm::warnings fastmm::lowlatency)
  target_compile_definitions(${tgt} PUBLIC
    $<$<BOOL:${FASTMM_HOT_PATH_NOALLOC_CHECK}>:FASTMM_HOT_PATH_NOALLOC_CHECK=1>)
endfunction()

# Prefer lld with clang when available (faster links, better LTO); never required.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  find_program(FASTMM_LLD NAMES ld.lld ld.lld-20 ld.lld-19 ld.lld-18 ld.lld-17)
  if(FASTMM_LLD)
    add_link_options("--ld-path=${FASTMM_LLD}")
    message(STATUS "fastmm: using lld at ${FASTMM_LLD}")
  endif()
endif()
