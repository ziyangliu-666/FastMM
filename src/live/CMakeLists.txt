# fastmm_live: the live trading runtime. session.cpp wires venues, rings, the engine, the journal and
# the status segment into the thread model and runs a session (run_live); cli.cpp is the fastmm-live
# command line (fastmm::cli::live). Strategies come from the registry: the built-ins
# (fastmm::strategies) and the modules an app passes to cli::live.
file(GLOB FASTMM_LIVE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
file(GLOB FASTMM_LIVE_HEADERS CONFIGURE_DEPENDS
  ${PROJECT_SOURCE_DIR}/include/fastmm/live/*.hpp
  ${PROJECT_SOURCE_DIR}/include/fastmm/cli/live.hpp)
add_library(fastmm_live STATIC ${FASTMM_LIVE_SOURCES} ${FASTMM_LIVE_HEADERS})
add_library(fastmm::live ALIAS fastmm_live)
set_target_properties(fastmm_live PROPERTIES EXPORT_NAME live)
fastmm_target_defaults(fastmm_live)
target_link_libraries(fastmm_live PUBLIC fastmm::venues fastmm::strategies fastmm::store)
