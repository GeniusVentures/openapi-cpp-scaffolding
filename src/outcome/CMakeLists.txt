add_library(outcome INTERFACE)

target_include_directories(outcome
    INTERFACE
    "${_THIRDPARTY_BUILD_DIR}/libp2p/include"
)

target_link_libraries(outcome
    INTERFACE
    Boost::headers
)

project_install(outcome)
