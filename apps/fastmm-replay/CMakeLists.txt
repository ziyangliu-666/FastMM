file(GLOB_RECURSE _src CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
fastmm_app(fastmm-replay ${_src})
target_link_libraries(fastmm-replay PRIVATE fastmm::backtest)
