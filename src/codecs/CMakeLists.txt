# fastmm::codecs (M3): Nasdaq ITCH 5.0 / MoldUDP64 / OUCH / SoupBinTCP, and -- behind
# FASTMM_CODEC_FIX / FASTMM_CODEC_MDP3 -- FIX 4.4 and CME MDP 3.0 SBE. Wire codecs and session
# layers only depend on core (no OpenSSL): transports plug them into the net stack, simulators and
# replay drive them directly. One subdirectory per protocol family.
file(GLOB_RECURSE FASTMM_CODECS_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
file(GLOB_RECURSE FASTMM_CODECS_HEADERS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/fastmm/codecs/*.hpp)
# No venue drives FIX or MDP3 yet, so the default build leaves them out (the headers stay in the
# tree). Their tests and benchmarks follow the same option; see the top-level CMakeLists.txt.
if(NOT FASTMM_CODEC_FIX)
  list(FILTER FASTMM_CODECS_SOURCES EXCLUDE REGEX "/fix/")
  list(FILTER FASTMM_CODECS_HEADERS EXCLUDE REGEX "/fix/")
endif()
if(NOT FASTMM_CODEC_MDP3)
  list(FILTER FASTMM_CODECS_SOURCES EXCLUDE REGEX "/mdp3/")
  list(FILTER FASTMM_CODECS_HEADERS EXCLUDE REGEX "/mdp3/")
endif()
add_library(fastmm_codecs STATIC ${FASTMM_CODECS_SOURCES} ${FASTMM_CODECS_HEADERS})
add_library(fastmm::codecs ALIAS fastmm_codecs)
set_target_properties(fastmm_codecs PROPERTIES EXPORT_NAME codecs)
fastmm_target_defaults(fastmm_codecs)
target_link_libraries(fastmm_codecs PUBLIC fastmm::core)
if(FASTMM_CODEC_FIX)
  target_compile_definitions(fastmm_codecs PUBLIC FASTMM_CODEC_FIX=1)
endif()
if(FASTMM_CODEC_MDP3)
  target_compile_definitions(fastmm_codecs PUBLIC FASTMM_CODEC_MDP3=1)
endif()
