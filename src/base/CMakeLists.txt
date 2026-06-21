add_executable(genius_ai_server
    main.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/config/GsmConfig.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/storage/GsmStorageManager.cpp"
)

target_include_directories(genius_ai_server PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../scaffold/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../os/${TARGET_PLATFORM}"
)

target_link_libraries(genius_ai_server
    PRIVATE
    component_factory
    storage
    auth_utils
    Boost::system
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
)

if(TARGET backend_apis)
    add_dependencies(genius_ai_server backend_apis)
endif()

# Copy swagger UI next to the binary — re-copies when the source changes.
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/swagger/index.html"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/swagger"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/swagger/index.html"
        "${CMAKE_CURRENT_BINARY_DIR}/swagger/index.html"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/swagger/index.html"
)
add_custom_target(swagger_ui DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/swagger/index.html")
add_dependencies(genius_ai_server swagger_ui)

# Install swagger next to the binary so it finds it at g_exeDir + "/swagger/".
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/swagger/index.html"
    DESTINATION "${CMAKE_INSTALL_BINDIR}/swagger")

project_install(genius_ai_server)
