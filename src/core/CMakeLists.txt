file(GLOB_RECURSE FASTMM_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
file(GLOB_RECURSE FASTMM_CORE_HEADERS CONFIGURE_DEPENDS
  ${PROJECT_SOURCE_DIR}/include/fastmm/core/*.hpp
  ${PROJECT_SOURCE_DIR}/include/fastmm/config/*.hpp
  ${PROJECT_SOURCE_DIR}/include/fastmm/strategies/*.hpp)
add_library(fastmm_core STATIC ${FASTMM_CORE_SOURCES} ${FASTMM_CORE_HEADERS})
add_library(fastmm::core ALIAS fastmm_core)
set_target_properties(fastmm_core PROPERTIES EXPORT_NAME core)
fastmm_target_defaults(fastmm_core)
target_include_directories(fastmm_core PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(fastmm_core PUBLIC fmt::fmt Threads::Threads PRIVATE $<BUILD_INTERFACE:tomlplusplus::tomlplusplus>)
target_compile_definitions(fastmm_core PRIVATE
  FASTMM_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  FASTMM_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
  FASTMM_NATIVE_ARCH=$<BOOL:${FASTMM_NATIVE_ARCH}>
  FASTMM_LTO=$<BOOL:${CMAKE_INTERPROCEDURAL_OPTIMIZATION}>)
