cmake_minimum_required(VERSION 3.16)

option(NO_ISA_EXTENSIONS "Disable ISA extensions" OFF)

if(DEFINED NO_STATISTICS AND NO_STATISTICS)
    message(FATAL_ERROR "tracy-query requires statistics; NO_STATISTICS=ON is not supported")
endif()
set(NO_STATISTICS OFF)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/version.cmake)

project(
    tracy-query
    LANGUAGES C CXX
    VERSION ${TRACY_VERSION_STRING}
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/config.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/vendor.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/server.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/analysis.cmake)

file(READ ${CMAKE_CURRENT_LIST_DIR}/schema/tracy-query-v1.schema.json TRACY_QUERY_SCHEMA_JSON)
file(READ ${CMAKE_CURRENT_LIST_DIR}/schema/coverage-v1.json TRACY_QUERY_COVERAGE_JSON)
configure_file(src/TracyEmbeddedData.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/TracyEmbeddedData.cpp @ONLY)

add_library(TracyQueryProtocol STATIC
    src/TracyMcpServer.cpp
    src/TracyMcpServer.hpp
    src/TracyPng.cpp
    src/TracyPng.hpp
    src/TracyQueryService.cpp
    src/TracyQueryService.hpp
    src/TracySessionManager.cpp
    src/TracySessionManager.hpp
    src/TracyEmbeddedData.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/generated/TracyEmbeddedData.cpp
)
target_include_directories(TracyQueryProtocol PUBLIC src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_include_directories(TracyQueryProtocol PRIVATE ${glfw_SOURCE_DIR}/deps)
target_link_libraries(TracyQueryProtocol PUBLIC TracyAnalysis nlohmann_json::nlohmann_json)
target_compile_features(TracyQueryProtocol PUBLIC cxx_std_20)

add_executable(tracy-query src/main.cpp)
target_link_libraries(tracy-query PRIVATE TracyQueryProtocol)
set_property(DIRECTORY ${CMAKE_CURRENT_LIST_DIR} PROPERTY VS_STARTUP_PROJECT tracy-query)

install(TARGETS tracy-query DESTINATION ${CMAKE_INSTALL_BINDIR})

include(CTest)
if(BUILD_TESTING)
    add_executable(tracy-query-contract-tests tests/ProtocolTests.cpp)
    target_link_libraries(tracy-query-contract-tests PRIVATE TracyAnalysis nlohmann_json::nlohmann_json)
    target_compile_definitions(tracy-query-contract-tests PRIVATE
        TRACY_QUERY_SCHEMA_PATH="${CMAKE_CURRENT_LIST_DIR}/schema/tracy-query-v1.schema.json"
        TRACY_QUERY_COVERAGE_PATH="${CMAKE_CURRENT_LIST_DIR}/schema/coverage-v1.json"
    )
    add_test(NAME tracy-query-contract COMMAND tracy-query-contract-tests)

    add_executable(tracy-query-mcp-transcript-tests tests/McpTranscriptTests.cpp)
    target_link_libraries(tracy-query-mcp-transcript-tests PRIVATE TracyQueryProtocol)
    add_test(NAME tracy-query-mcp-transcript COMMAND tracy-query-mcp-transcript-tests)

    add_test(NAME tracy-query-version COMMAND tracy-query --version)
    add_test(NAME tracy-query-doctor COMMAND tracy-query --doctor)

    set(TRACY_ACCEPTANCE_TRACE_ROOT "$ENV{TRACY_ACCEPTANCE_TRACE_ROOT}" CACHE PATH "Directory containing local Tracy acceptance captures")
    if(TRACY_ACCEPTANCE_TRACE_ROOT)
        set(TRACY_ACCEPTANCE_TRACE "${TRACY_ACCEPTANCE_TRACE_ROOT}/Test-GPUMemory-Fresh1.tracy")
        if(EXISTS "${TRACY_ACCEPTANCE_TRACE}")
            add_test(NAME tracy-query-worker-smoke COMMAND tracy-query
                --allow-root "${TRACY_ACCEPTANCE_TRACE_ROOT}"
                --trace "${TRACY_ACCEPTANCE_TRACE}"
                --request "${CMAKE_CURRENT_LIST_DIR}/tests/requests/overview.json"
            )

            add_executable(tracy-query-mcp-worker-integration-tests tests/McpWorkerIntegrationTests.cpp)
            target_link_libraries(tracy-query-mcp-worker-integration-tests PRIVATE TracyQueryProtocol)
            target_compile_definitions(tracy-query-mcp-worker-integration-tests PRIVATE TRACY_ACCEPTANCE_TRACE="${TRACY_ACCEPTANCE_TRACE}")
            add_test(NAME tracy-query-mcp-worker-integration COMMAND tracy-query-mcp-worker-integration-tests)
        endif()
    endif()
endif()
