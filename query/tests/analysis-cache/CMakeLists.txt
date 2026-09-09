cmake_minimum_required(VERSION 3.20)
project(tracy-analysis-cache-unit LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(TRACY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(JN_JSON_INCLUDE "" CACHE PATH "Existing nlohmann JSON include directory (read only)")
if(NOT EXISTS "${JN_JSON_INCLUDE}/nlohmann/json.hpp")
    message(FATAL_ERROR "JN_JSON_INCLUDE must point to the existing JSON headers; no download is performed")
endif()
add_executable(tracy-analysis-cache-table-tests ../AnalysisCacheTableTests.cpp
    "${TRACY_ROOT}/analysis/TracyAnalysisCacheTable.cpp"
    "${TRACY_ROOT}/analysis/TracyAnalysisCacheSort.cpp"
    "${TRACY_ROOT}/analysis/TracyHash.cpp")
target_include_directories(tracy-analysis-cache-table-tests PRIVATE "${TRACY_ROOT}/analysis" "${JN_JSON_INCLUDE}")
if(MSVC)
    target_compile_definitions(tracy-analysis-cache-table-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(tracy-analysis-cache-table-tests PRIVATE /UNDEBUG /utf-8 /EHsc)
    target_link_libraries(tracy-analysis-cache-table-tests PRIVATE Bcrypt)
else()
    target_compile_options(tracy-analysis-cache-table-tests PRIVATE -UNDEBUG)
endif()
include(CTest)
add_test(NAME tracy-analysis-cache-table COMMAND tracy-analysis-cache-table-tests)
add_executable(tracy-analysis-cache-query-tests ../AnalysisCacheQueryTests.cpp
    "${TRACY_ROOT}/analysis/TracyAnalysisCacheQuery.cpp"
    "${TRACY_ROOT}/analysis/TracyAnalysisCacheTable.cpp"
    "${TRACY_ROOT}/analysis/TracyHash.cpp")
target_include_directories(tracy-analysis-cache-query-tests PRIVATE "${TRACY_ROOT}/analysis" "${JN_JSON_INCLUDE}")
if(MSVC)
    target_compile_definitions(tracy-analysis-cache-query-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(tracy-analysis-cache-query-tests PRIVATE /UNDEBUG /utf-8 /EHsc)
    target_link_libraries(tracy-analysis-cache-query-tests PRIVATE Bcrypt)
else()
    target_compile_options(tracy-analysis-cache-query-tests PRIVATE -UNDEBUG)
endif()
add_test(NAME tracy-analysis-cache-query COMMAND tracy-analysis-cache-query-tests)
add_executable(tracy-analysis-workspace-budget-tests ../AnalysisWorkspaceBudgetTests.cpp
    "${TRACY_ROOT}/analysis/TracyAnalysisProcessMemory.cpp"
    "${TRACY_ROOT}/analysis/TracyAnalysisCacheTable.cpp"
    "${TRACY_ROOT}/analysis/TracyAnalysisCacheSort.cpp"
    "${TRACY_ROOT}/analysis/TracyHash.cpp")
target_include_directories(tracy-analysis-workspace-budget-tests PRIVATE "${TRACY_ROOT}/analysis" "${JN_JSON_INCLUDE}")
if(MSVC)
    target_compile_definitions(tracy-analysis-workspace-budget-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(tracy-analysis-workspace-budget-tests PRIVATE /UNDEBUG /utf-8 /EHsc)
    target_link_libraries(tracy-analysis-workspace-budget-tests PRIVATE Bcrypt Psapi)
else()
    target_compile_options(tracy-analysis-workspace-budget-tests PRIVATE -UNDEBUG)
endif()
add_test(NAME tracy-analysis-workspace-budget COMMAND tracy-analysis-workspace-budget-tests)
