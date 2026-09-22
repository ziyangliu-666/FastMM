# rx_backend = "dpdk" (FASTMM_WITH_DPDK=ON): the only translation unit that includes DPDK headers.
# The static link line keeps its --whole-archive group (PMDs register from constructors).
add_library(fastmm_dpdk STATIC dpdk_datagram_source.cpp)
add_library(fastmm::dpdk ALIAS fastmm_dpdk)
set_target_properties(fastmm_dpdk PROPERTIES EXPORT_NAME dpdk)
fastmm_target_defaults(fastmm_dpdk)
target_include_directories(fastmm_dpdk SYSTEM PRIVATE ${DPDK_STATIC_INCLUDE_DIRS})
target_compile_options(fastmm_dpdk PRIVATE ${FASTMM_DPDK_CFLAGS})
target_compile_definitions(fastmm_dpdk PRIVATE FASTMM_HAS_DPDK=1)
target_link_libraries(fastmm_dpdk PUBLIC fastmm::core ${DPDK_STATIC_LDFLAGS})
