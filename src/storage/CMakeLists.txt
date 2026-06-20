add_library(storage STATIC
    StorageError.cpp
    KeyBuilder.cpp
    RocksDBEngine.cpp
)

target_include_directories(storage
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
)

target_link_libraries(storage
    PUBLIC
    outcome
    Boost::headers
    spdlog::spdlog
)

if(TARGET RocksDB::rocksdb)
    target_link_libraries(storage PUBLIC RocksDB::rocksdb)
endif()

if(TARGET Snappy::snappy)
    target_link_libraries(storage PUBLIC Snappy::snappy)
endif()

project_install(storage)
