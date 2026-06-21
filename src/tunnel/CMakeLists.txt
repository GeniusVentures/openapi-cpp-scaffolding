#
# tunnel/CMakeLists.txt
# Build configuration for the tunnel plugin shared library.
# This plugin manages cloudflared as a subprocess for Cloudflare Tunnel support.
#

set(TUNNEL_SOURCES
    SubprocessManager.cpp
    BinaryDiscovery.cpp
    CloudflareClient.cpp
    tunnel_plugin.cpp
)

add_library(tunnel_api SHARED ${TUNNEL_SOURCES})

target_include_directories(tunnel_api PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(tunnel_api PRIVATE
    component_factory
    storage
    Boost::filesystem
    Boost::headers
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    OpenSSL::SSL
    spdlog::spdlog
)

set_target_properties(tunnel_api PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    PREFIX ""
)

install(TARGETS tunnel_api
    LIBRARY DESTINATION plugins
    RUNTIME DESTINATION plugins
)
