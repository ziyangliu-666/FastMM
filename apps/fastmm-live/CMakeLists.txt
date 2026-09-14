file(GLOB_RECURSE _src CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
fastmm_app(fastmm-live ${_src})
target_link_libraries(fastmm-live PRIVATE fastmm::venues fastmm::strategies)
