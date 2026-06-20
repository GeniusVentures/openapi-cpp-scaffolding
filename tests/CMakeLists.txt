find_package(GTest CONFIG REQUIRED)
include(CTest)

enable_testing()

function(add_unittest test_name)
    add_executable(${test_name} ${ARGN})
    target_link_libraries(${test_name}
        PRIVATE
        component_factory
        storage
        GTest::gtest_main
    )
    target_include_directories(${test_name}
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    )
    add_test(
        NAME ${test_name}
        COMMAND ${test_name}
    )
endfunction()

add_unittest(test_fnv1a test_fnv1a.cpp)
add_unittest(test_plugin_manager test_plugin_manager.cpp)
add_unittest(test_mock_plugin test_mock_plugin.cpp)
add_unittest(test_storage_engine test_storage_engine.cpp)
add_unittest(test_change_callback test_change_callback.cpp)

# Auth utilities tests (JWT + PBKDF2)
add_executable(test_auth
    test_auth.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/identity/auth_utils.cpp"
)
target_link_libraries(test_auth PRIVATE
    component_factory
    storage
    GTest::gtest_main
    OpenSSL::Crypto
)
target_include_directories(test_auth PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/identity/../jwt-cpp/include"
)
add_test(NAME test_auth COMMAND test_auth)

# Auth login handler integration tests (login, logout, me, refresh against real RocksDB)
add_executable(test_auth_login
    test_auth_login.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/identity/auth_utils.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/identity/identity_auth_handlers.cpp"
)
target_link_libraries(test_auth_login PRIVATE
    component_factory
    storage
    GTest::gtest_main
    OpenSSL::Crypto
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)
target_include_directories(test_auth_login PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/identity/../jwt-cpp/include"
)
add_test(NAME test_auth_login COMMAND test_auth_login)

# S3Client tests
add_executable(test_s3_client
    test_s3_client.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/archive/S3Client.cpp"
)
target_link_libraries(test_s3_client PRIVATE
    component_factory
    storage
    GTest::gtest_main
    Boost::headers
    OpenSSL::Crypto
    OpenSSL::SSL
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)
target_include_directories(test_s3_client PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
add_test(NAME test_s3_client COMMAND test_s3_client)

# StreamingWorker tests
add_executable(test_streaming_worker
    test_streaming_worker.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/archive/S3Client.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/archive/StreamingWorker.cpp"
)
target_link_libraries(test_streaming_worker PRIVATE
    component_factory
    storage
    GTest::gtest_main
    Boost::headers
    OpenSSL::Crypto
    OpenSSL::SSL
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)
target_include_directories(test_streaming_worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
add_test(NAME test_streaming_worker COMMAND test_streaming_worker)

# Archive admin API handler integration tests
add_executable(test_archive_handlers
    test_archive_handlers.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/archive/S3Client.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/archive/StreamingWorker.cpp"
)
target_link_libraries(test_archive_handlers PRIVATE
    component_factory
    storage
    GTest::gtest_main
    Boost::headers
    OpenSSL::Crypto
    OpenSSL::SSL
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)
target_include_directories(test_archive_handlers PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
add_test(NAME test_archive_handlers COMMAND test_archive_handlers)

# Tunnel setup wizard handler integration tests
add_executable(test_tunnel_setup
    test_tunnel_setup.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/tunnel/SubprocessManager.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/tunnel/BinaryDiscovery.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/tunnel/CloudflareClient.cpp"
)
target_link_libraries(test_tunnel_setup PRIVATE
    component_factory
    storage
    GTest::gtest_main
    Boost::headers
    Boost::filesystem
    Boost::system
    OpenSSL::Crypto
    OpenSSL::SSL
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)
target_include_directories(test_tunnel_setup PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
add_test(NAME test_tunnel_setup COMMAND test_tunnel_setup)
