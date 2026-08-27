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
)
target_include_directories(test_auth_login PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/identity/../jwt-cpp/include"
)
target_link_libraries(test_auth_login PRIVATE nlohmann_json::nlohmann_json)
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
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    OpenSSL::SSL
    spdlog::spdlog
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
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    OpenSSL::SSL
    spdlog::spdlog
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
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    OpenSSL::SSL
    spdlog::spdlog
)
target_include_directories(test_tunnel_setup PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
add_test(NAME test_tunnel_setup COMMAND test_tunnel_setup)

# --- Phase 7: generated OrderEntry store Google Test (07-04, D-17) ----------
# Compiles the RENDERED OrderEntryStore.cpp (frontend/generated) against the
# commerce OpenAPI model sources it reuses (D-05). No per-store .jinja2 test
# template exists by design. The render-before-test dependency edge is created
# by the parent frontend/CMakeLists.txt when this directory configures first
# (normal order: root adds backend/scaffold BEFORE frontend); the TARGET guard
# below covers the reverse order. The EXISTS guard mirrors the frontend side's
# condition so a missing frontend scaffold submodule skips registration.
set(_order_entry_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
if(EXISTS "${_order_entry_repo_root}/frontend/scaffold/tools/scaffold_codegen/generators/cpp_store.py")
    add_unittest(test_order_entry_store
        test_order_entry_store.cpp
        "${_order_entry_repo_root}/frontend/generated/pos/cpp/order_entry/OrderEntryStore.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/commerce/generated/model/Order.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/commerce/generated/model/OrderLine.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/commerce/generated/model/OrderDiscount.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/commerce/generated/model/ServiceCharge.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/commerce/generated/model/Money.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/commerce/generated/model/Helpers.cpp"
    )
    target_include_directories(test_order_entry_store PRIVATE
        "${_order_entry_repo_root}/frontend/generated/pos/cpp/order_entry"
        "${_order_entry_repo_root}/frontend/generated/shared/cpp"
    )
    set_source_files_properties(
        "${_order_entry_repo_root}/frontend/generated/pos/cpp/order_entry/OrderEntryStore.cpp"
        PROPERTIES GENERATED TRUE
    )
    target_link_libraries(test_order_entry_store PRIVATE nlohmann_json::nlohmann_json)
    if(TARGET frontend_generate_cpp_templates)
        add_dependencies(test_order_entry_store frontend_generate_cpp_templates)
    endif()
endif()
