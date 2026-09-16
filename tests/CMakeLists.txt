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

# POS override handler behavior tests (Phase 2 plan 02-04, D-04)
# Compiles the handler sources + generated models directly into the test
# executable (NEVER links the plugin dylibs). The generated plugin base
# classes are header-inline. Shared models (Money/Pagination/Helpers) compile
# exactly once — restaurant's copies — to avoid duplicate symbols; commerce's
# copies are API-identical (comment-only diffs, same include guards).
add_executable(test_pos_handlers
    test_pos_handlers.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/restaurant/restaurant_pos_handlers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/commerce/commerce_pos_handlers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/restaurant/generated/model/MenuCategory.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/restaurant/generated/model/MenuCategoryCreate.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/restaurant/generated/model/MenuItem.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/restaurant/generated/model/MenuItemCreate.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/restaurant/generated/model/ModifierGroup.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/restaurant/generated/model/ModifierGroupCreate.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/restaurant/generated/model/Modifier.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/restaurant/generated/model/KitchenTicket.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/restaurant/generated/model/KitchenTicketCreate.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/restaurant/generated/model/Money.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/restaurant/generated/model/Pagination.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/restaurant/generated/model/Helpers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/commerce/generated/model/Order.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/commerce/generated/model/OrderCreate.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/commerce/generated/model/OrderLine.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/commerce/generated/model/OrderDiscount.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/commerce/generated/model/ServiceCharge.cpp"
)
target_link_libraries(test_pos_handlers PRIVATE
    component_factory
    storage
    GTest::gtest_main
    Boost::headers
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)
target_include_directories(test_pos_handlers PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/restaurant/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/commerce/generated"
)
add_test(NAME test_pos_handlers COMMAND test_pos_handlers)

# Locations setup contract tests (Phase 3 plan 03-03, TAX-01..03)
# Consumes the committed parent-repo dev setup JSON as its expected-values
# source via the SETUP_DEV_JSON compile definition (same parent-repo coupling
# precedent as src/CMakeLists.txt and test_order_entry_store's repo root).
# Generated model sources compile directly into the executable (NEVER links
# the plugin dylibs); the LocationsPlugin base class is header-inline.
add_executable(test_locations_setup
    test_locations_setup.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/locations/generated/model/Location.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/locations/generated/model/Address.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/locations/generated/model/ContactPoint.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/locations/generated/model/Location_hours_inner.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/locations/generated/model/Helpers.cpp"
)
target_link_libraries(test_locations_setup PRIVATE
    component_factory
    storage
    GTest::gtest_main
    nlohmann_json::nlohmann_json
)
target_include_directories(test_locations_setup PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/locations/generated"
)
target_compile_definitions(test_locations_setup PRIVATE
    SETUP_DEV_JSON="${CMAKE_CURRENT_SOURCE_DIR}/../../../setup/environments/dev.json"
)
add_test(NAME test_locations_setup COMMAND test_locations_setup)

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
