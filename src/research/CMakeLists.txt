# fastmm_research: the feature and forward-markout extractor, and the signal evaluation on top of
# it. Reads an MdSource through fastmm::backtest's data-source registry; nothing in the engine or
# the backtest depends on it.
file(GLOB_RECURSE FASTMM_RESEARCH_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
file(GLOB_RECURSE FASTMM_RESEARCH_HEADERS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/fastmm/research/*.hpp)
add_library(fastmm_research STATIC ${FASTMM_RESEARCH_SOURCES} ${FASTMM_RESEARCH_HEADERS})
add_library(fastmm::research ALIAS fastmm_research)
set_target_properties(fastmm_research PROPERTIES EXPORT_NAME research)
fastmm_target_defaults(fastmm_research)
target_link_libraries(fastmm_research PUBLIC fastmm::backtest)
