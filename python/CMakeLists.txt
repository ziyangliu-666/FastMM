file(GLOB _py_src CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
pybind11_add_module(fastmm_py MODULE ${_py_src})
fastmm_target_defaults(fastmm_py)
target_link_libraries(fastmm_py PRIVATE fastmm::backtest)
set_target_properties(fastmm_py PROPERTIES OUTPUT_NAME _core)
if(SKBUILD)
  # Component-scoped so the wheel holds only the module (pyproject install.components):
  # third-party install rules (fmt, toml++) must not leak headers/archives into site-packages.
  install(TARGETS fastmm_py LIBRARY DESTINATION fastmm COMPONENT python)
else()
  # In-tree use: python -c "import sys; sys.path.insert(0,'python'); import fastmm"
  set_target_properties(fastmm_py PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/python/fastmm)
endif()
