fastmm_app(fastmm-pnl main.cpp)
target_link_libraries(fastmm-pnl PRIVATE fastmm::store)
target_link_libraries(fastmm-pnl PRIVATE fastmm_cli11)
