# fastmm::codecs (M3): FIX 4.4, Nasdaq ITCH 5.0 / MoldUDP64 / OUCH / SoupBinTCP, CME MDP 3.0 SBE.
# Wire codecs and session layers only depend on core (no OpenSSL): transports plug them into the
# net stack, simulators and replay drive them directly. One subdirectory per protocol family.
file(GLOB_RECURSE FASTMM_CODECS_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
file(GLOB_RECURSE FASTMM_CODECS_HEADERS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/fastmm/codecs/*.hpp)
add_library(fastmm_codecs STATIC ${FASTMM_CODECS_SOURCES} ${FASTMM_CODECS_HEADERS})
add_library(fastmm::codecs ALIAS fastmm_codecs)
set_target_properties(fastmm_codecs PROPERTIES EXPORT_NAME codecs)
fastmm_target_defaults(fastmm_codecs)
target_link_libraries(fastmm_codecs PUBLIC fastmm::core)
