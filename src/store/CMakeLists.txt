# fastmm_store: the queryable record of a session (fills, orders, positions, PnL, kill events),
# written from a thread that is not the engine's. Backends are chosen by name at runtime; the
# SQLite one ships here. The journal (fastmm::core) stays the byte-exact replay stream.
file(GLOB_RECURSE FASTMM_STORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
file(GLOB_RECURSE FASTMM_STORE_HEADERS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/fastmm/store/*.hpp)
add_library(fastmm_store STATIC ${FASTMM_STORE_SOURCES} ${FASTMM_STORE_HEADERS})
add_library(fastmm::store ALIAS fastmm_store)
set_target_properties(fastmm_store PROPERTIES EXPORT_NAME store)
fastmm_target_defaults(fastmm_store)
target_link_libraries(fastmm_store PUBLIC fastmm::core PRIVATE sqlite3)
