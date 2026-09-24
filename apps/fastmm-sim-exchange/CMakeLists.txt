file(GLOB_RECURSE _src CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
fastmm_app(fastmm-sim-exchange ${_src})
target_link_libraries(fastmm-sim-exchange PRIVATE fastmm::sim_server)
target_link_libraries(fastmm-sim-exchange PRIVATE fastmm_cli11)
