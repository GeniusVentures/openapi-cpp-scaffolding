# Copy OpenAPI specs next to the server binary at configure time.
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/" DESTINATION "${CMAKE_BINARY_DIR}/../src/base/json"
     FILES_MATCHING PATTERN "*.json")

# Install specs for packaging.
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/"
    DESTINATION backend
    FILES_MATCHING PATTERN "*.json"
)
