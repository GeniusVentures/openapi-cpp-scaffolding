add_library(component_factory
    CComponentFactory.cpp
    PluginManager.cpp
)

target_include_directories(component_factory
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../os/${TARGET_PLATFORM}"
)

target_link_libraries(component_factory
    PUBLIC
    outcome
    Boost::headers
    spdlog::spdlog
)

project_install(component_factory)
