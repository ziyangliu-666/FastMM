file(GLOB_RECURSE _src CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
fastmm_app(fastmm-sim-itch ${_src})
target_link_libraries(fastmm-sim-itch PRIVATE fastmm::sim_itch)
