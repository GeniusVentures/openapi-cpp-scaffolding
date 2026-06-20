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

project_install(genius_ai_server)
