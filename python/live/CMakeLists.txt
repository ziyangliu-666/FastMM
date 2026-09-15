# fastmm_live._live: the extension module of the fastmm-live wheel (pyproject.toml in this directory).
# It links the network stack, the venue connectors and fastmm::live, and OpenSSL statically with
# FASTMM_OPENSSL_STATIC. exports.map keeps every symbol but PyInit__live out of the dynamic symbol
# table, so the OpenSSL and FastMM code inside neither interposes on nor is interposed by another
# OpenSSL or fastmm._core in the same process.

# The self-test handshake uses the test certificate (CN=localhost), compiled into the module.
file(READ ${PROJECT_SOURCE_DIR}/tests/fixtures/tls/cert.pem FASTMM_SELF_TEST_CERT_PEM)
file(READ ${PROJECT_SOURCE_DIR}/tests/fixtures/tls/key.pem FASTMM_SELF_TEST_KEY_PEM)
configure_file(src/self_test_pem.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/self_test_pem.hpp @ONLY)

pybind11_add_module(fastmm_live_py MODULE src/module.cpp src/session.cpp)
fastmm_target_defaults(fastmm_live_py)
# python/src/hot_program.hpp: the compiled hot program, shared with fastmm._core.
target_include_directories(fastmm_live_py PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated
  ${PROJECT_SOURCE_DIR}/python/src)
target_link_libraries(fastmm_live_py PRIVATE fastmm::live fastmm::venues fastmm::net)
target_compile_definitions(fastmm_live_py PRIVATE
  FASTMM_OPENSSL_STATIC=$<BOOL:${FASTMM_OPENSSL_STATIC}>)
target_link_options(fastmm_live_py PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map")
set_property(TARGET fastmm_live_py APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/exports.map)
set_target_properties(fastmm_live_py PROPERTIES OUTPUT_NAME _live)
if(SKBUILD)
  install(TARGETS fastmm_live_py LIBRARY DESTINATION fastmm_live COMPONENT python)
else()
  # In-tree use: PYTHONPATH=python/live (fastmm must be importable too)
  set_target_properties(fastmm_live_py PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/fastmm_live)
endif()
