fastmm_app(fastmm-top main.cpp metrics_server.cpp)
target_link_libraries(fastmm-top PRIVATE fastmm::core)
