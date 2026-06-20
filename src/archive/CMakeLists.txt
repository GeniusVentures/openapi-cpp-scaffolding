#
# archive/CMakeLists.txt
# Build configuration for the archive plugin shared library.
# This plugin streams RocksDB changes to S3-compatible storage.
#

#

set(ARCHIVE_SOURCES
    archive_plugin.cpp
    S3Client.cpp
    StreamingWorker.cpp
)

add_library(archive_api SHARED ${ARCHIVE_SOURCES})

target_include_directories(archive_api PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(archive_api PRIVATE
    component_factory
    storage
    Boost::headers
    OpenSSL::Crypto
    OpenSSL::SSL
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

set_target_properties(archive_api PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    PREFIX ""
)

install(TARGETS archive_api
    LIBRARY DESTINATION plugins
    RUNTIME DESTINATION plugins
)
