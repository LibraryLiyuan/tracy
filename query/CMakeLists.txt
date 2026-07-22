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
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/analysis.cmake)

include(CTest)
if(BUILD_TESTING)
    add_executable(tracy-query-contract-tests tests/ProtocolTests.cpp)
    target_link_libraries(tracy-query-contract-tests PRIVATE TracyAnalysis nlohmann_json::nlohmann_json)
    target_compile_definitions(tracy-query-contract-tests PRIVATE
        TRACY_QUERY_SCHEMA_PATH="${CMAKE_CURRENT_LIST_DIR}/schema/tracy-query-v1.schema.json"
        TRACY_QUERY_COVERAGE_PATH="${CMAKE_CURRENT_LIST_DIR}/schema/coverage-v1.json"
    )
    add_test(NAME tracy-query-contract COMMAND tracy-query-contract-tests)
endif()
