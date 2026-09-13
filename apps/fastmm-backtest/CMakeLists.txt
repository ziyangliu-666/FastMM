file(GLOB_RECURSE _src CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
fastmm_app(fastmm-backtest ${_src})
target_link_libraries(fastmm-backtest PRIVATE fastmm::backtest)
