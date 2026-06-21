add_executable(genius_ai_server
    main.cpp
)

target_link_libraries(genius_ai_server
    PRIVATE
    component_factory
    storage
    auth_utils
    Boost::system
    OpenSSL::Crypto
)

if(TARGET backend_apis)
    add_dependencies(genius_ai_server backend_apis)
endif()

# Copy swagger UI next to the binary at configure time.
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/swagger/" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/swagger")

project_install(genius_ai_server)
