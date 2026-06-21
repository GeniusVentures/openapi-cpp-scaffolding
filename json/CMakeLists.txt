# Copy OpenAPI specs next to the server binary at configure time.
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/" DESTINATION "${CMAKE_BINARY_DIR}/../src/base/json"
     FILES_MATCHING PATTERN "*.json")

# Install specs next to the binary (<prefix>/json/).
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/"
    DESTINATION json
    FILES_MATCHING PATTERN "*.json"
)
