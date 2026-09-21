# fastmm_sim: matching engine, latency models, virtual clock/transport (no network deps).
# The Binance-compatible sim *server* (sim/server/*) lives in fastmm_sim_server and the Nasdaq-style
# fastmm-sim-itch (sim/itch/*) in fastmm_sim_itch; both need fastmm::net.
file(GLOB FASTMM_SIM_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
file(GLOB FASTMM_SIM_HEADERS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/fastmm/sim/*.hpp)
add_library(fastmm_sim STATIC ${FASTMM_SIM_SOURCES} ${FASTMM_SIM_HEADERS})
add_library(fastmm::sim ALIAS fastmm_sim)
set_target_properties(fastmm_sim PROPERTIES EXPORT_NAME sim)
fastmm_target_defaults(fastmm_sim)
target_link_libraries(fastmm_sim PUBLIC fastmm::core)

if(FASTMM_BUILD_NET)
  file(GLOB FASTMM_SIM_SERVER_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/server/*.cpp)
  file(GLOB FASTMM_SIM_SERVER_HEADERS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/fastmm/sim/server/*.hpp)
  if(FASTMM_SIM_SERVER_SOURCES)
    add_library(fastmm_sim_server STATIC ${FASTMM_SIM_SERVER_SOURCES} ${FASTMM_SIM_SERVER_HEADERS})
    add_library(fastmm::sim_server ALIAS fastmm_sim_server)
    set_target_properties(fastmm_sim_server PROPERTIES EXPORT_NAME sim_server)
    fastmm_target_defaults(fastmm_sim_server)
    target_link_libraries(fastmm_sim_server PUBLIC fastmm::sim fastmm::net PRIVATE simdjson::simdjson)
  endif()
endif()

# fastmm_sim_itch_publisher: MatchingEngine effects -> ITCH 5.0 (ItchPublisher; no network).
add_library(fastmm_sim_itch_publisher STATIC ${CMAKE_CURRENT_SOURCE_DIR}/itch/itch_publisher.cpp
  ${PROJECT_SOURCE_DIR}/include/fastmm/sim/itch/itch_publisher.hpp)
add_library(fastmm::sim_itch_publisher ALIAS fastmm_sim_itch_publisher)
set_target_properties(fastmm_sim_itch_publisher PROPERTIES EXPORT_NAME sim_itch_publisher)
fastmm_target_defaults(fastmm_sim_itch_publisher)
target_link_libraries(fastmm_sim_itch_publisher PUBLIC fastmm::sim fastmm::codecs)

# fastmm_sim_itch: the fastmm-sim-itch simulator (multicast ITCH, re-requests, GLIMPSE, OUCH).
if(FASTMM_BUILD_NET)
  set(FASTMM_SIM_ITCH_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/itch/feed.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/itch/sessions.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/itch/sim_itch_config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/itch/sim_itch_server.cpp)
  add_library(fastmm_sim_itch STATIC ${FASTMM_SIM_ITCH_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/itch/sim_itch_impl.hpp
    ${PROJECT_SOURCE_DIR}/include/fastmm/sim/itch/sim_itch_config.hpp
    ${PROJECT_SOURCE_DIR}/include/fastmm/sim/itch/sim_itch_server.hpp)
  add_library(fastmm::sim_itch ALIAS fastmm_sim_itch)
  set_target_properties(fastmm_sim_itch PROPERTIES EXPORT_NAME sim_itch)
  fastmm_target_defaults(fastmm_sim_itch)
  target_link_libraries(fastmm_sim_itch PUBLIC fastmm::sim_itch_publisher fastmm::net)
endif()
