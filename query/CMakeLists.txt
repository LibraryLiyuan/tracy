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
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "Build zstd programs" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "Build zstd tests" FORCE)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/vendor.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/server.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/stream.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/analysis.cmake)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/schema/tracy-query-v1.schema.json
    ${CMAKE_CURRENT_LIST_DIR}/schema/coverage-v1.json
    ${CMAKE_CURRENT_LIST_DIR}/schema/coverage-fields-v1.json
    ${CMAKE_CURRENT_LIST_DIR}/schema/coverage-mcp-v1.json
    ${CMAKE_CURRENT_LIST_DIR}/schema/analysis-profile-v1.schema.json
    ${CMAKE_CURRENT_LIST_DIR}/schema/neutral-aggregate-v1.schema.json
    ${CMAKE_CURRENT_LIST_DIR}/schema/candidate-manifest-v1.schema.json
    ${CMAKE_CURRENT_LIST_DIR}/schema/analysis-scan-api-v1.schema.json
)
file(READ ${CMAKE_CURRENT_LIST_DIR}/schema/tracy-query-v1.schema.json TRACY_QUERY_SCHEMA_JSON)
file(READ ${CMAKE_CURRENT_LIST_DIR}/schema/coverage-v1.json TRACY_QUERY_COVERAGE_JSON)
file(READ ${CMAKE_CURRENT_LIST_DIR}/schema/coverage-fields-v1.json TRACY_QUERY_FIELD_COVERAGE_JSON)
file(READ ${CMAKE_CURRENT_LIST_DIR}/schema/coverage-mcp-v1.json TRACY_QUERY_MCP_COVERAGE_JSON)
file(READ ${CMAKE_CURRENT_LIST_DIR}/schema/analysis-profile-v1.schema.json TRACY_ANALYSIS_PROFILE_SCHEMA_JSON)
file(READ ${CMAKE_CURRENT_LIST_DIR}/schema/neutral-aggregate-v1.schema.json TRACY_NEUTRAL_AGGREGATE_SCHEMA_JSON)
file(READ ${CMAKE_CURRENT_LIST_DIR}/schema/candidate-manifest-v1.schema.json TRACY_CANDIDATE_MANIFEST_SCHEMA_JSON)
file(READ ${CMAKE_CURRENT_LIST_DIR}/schema/analysis-scan-api-v1.schema.json TRACY_ANALYSIS_SCAN_API_SCHEMA_JSON)
configure_file(src/TracyEmbeddedData.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/TracyEmbeddedData.cpp @ONLY)

add_library(TracyQueryProtocol STATIC
    src/TracyMcpServer.cpp
    src/TracyMcpServer.hpp
    src/TracyAnalysisScanManager.cpp
    src/TracyAnalysisScanManager.hpp
    src/TracyAnalysisScanCache.cpp
    src/TracyAnalysisScanCache.hpp
    src/TracyPng.cpp
    src/TracyPng.hpp
    src/TracyQueryIndex.cpp
    src/TracyQueryIndex.hpp
    src/TracyQueryService.cpp
    src/TracyQueryService.hpp
    src/TracySegmentTraceSource.cpp
    src/TracySegmentTraceSource.hpp
    src/TracySessionManager.cpp
    src/TracySessionManager.hpp
    src/TracyEmbeddedData.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/generated/TracyEmbeddedData.cpp
)
target_include_directories(TracyQueryProtocol PUBLIC src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_include_directories(TracyQueryProtocol PRIVATE ${glfw_SOURCE_DIR}/deps)
target_link_libraries(TracyQueryProtocol PUBLIC TracyAnalysis TracyServer TracyStreamCore nlohmann_json::nlohmann_json)
target_compile_features(TracyQueryProtocol PUBLIC cxx_std_20)

add_executable(tracy-query src/main.cpp)
target_link_libraries(tracy-query PRIVATE TracyQueryProtocol)
set_property(DIRECTORY ${CMAKE_CURRENT_LIST_DIR} PROPERTY VS_STARTUP_PROJECT tracy-query)

option(JN_STREAM_CONVERT_SEMANTIC_TOOLS "Build local stream-convert semantic acceptance tools" OFF)
if(JN_STREAM_CONVERT_SEMANTIC_TOOLS)
    add_executable(tracy-job-semantic-hash tests/JobSemanticHash.cpp)
    target_link_libraries(tracy-job-semantic-hash PRIVATE TracyQueryProtocol)
endif()

install(TARGETS tracy-query DESTINATION ${CMAKE_INSTALL_BINDIR})

include(CTest)
if(BUILD_TESTING)
    function(tracy_query_enable_assertions target)
        # Keep assertions active in Release test builds and avoid LTCG for the
        # deliberately large Worker integration translation unit. MSVC 19.5x
        # can otherwise ICE while optimizing the bundled zstd templates.
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
        if(MSVC)
            target_compile_options(${target} PRIVATE /UNDEBUG /GL-)
        else()
            target_compile_options(${target} PRIVATE -UNDEBUG)
        endif()
    endfunction()

    add_executable(tracy-job-evidence-read-tests tests/JobEvidenceReadTests.cpp)
    target_link_libraries(tracy-job-evidence-read-tests PRIVATE TracyQueryProtocol)
    add_test(NAME tracy-job-evidence-read COMMAND tracy-job-evidence-read-tests)

    add_executable(tracy-query-contract-tests tests/ProtocolTests.cpp)
    target_link_libraries(tracy-query-contract-tests PRIVATE TracyQueryProtocol)
    target_compile_definitions(tracy-query-contract-tests PRIVATE
        TRACY_QUERY_SCHEMA_PATH="${CMAKE_CURRENT_LIST_DIR}/schema/tracy-query-v1.schema.json"
        TRACY_QUERY_COVERAGE_PATH="${CMAKE_CURRENT_LIST_DIR}/schema/coverage-v1.json"
        TRACY_QUERY_FIELD_COVERAGE_PATH="${CMAKE_CURRENT_LIST_DIR}/schema/coverage-fields-v1.json"
        TRACY_QUERY_MCP_COVERAGE_PATH="${CMAKE_CURRENT_LIST_DIR}/schema/coverage-mcp-v1.json"
    )
    tracy_query_enable_assertions(tracy-query-contract-tests)
    add_test(NAME tracy-query-contract COMMAND tracy-query-contract-tests)

    add_executable(tracy-query-deterministic-scan-contract-tests tests/DeterministicScanTests.cpp)
    target_link_libraries(tracy-query-deterministic-scan-contract-tests PRIVATE TracyQueryProtocol)
    target_compile_definitions(tracy-query-deterministic-scan-contract-tests PRIVATE
        TRACY_ANALYSIS_PROFILE_SCHEMA_PATH="${CMAKE_CURRENT_LIST_DIR}/schema/analysis-profile-v1.schema.json"
        TRACY_NEUTRAL_AGGREGATE_SCHEMA_PATH="${CMAKE_CURRENT_LIST_DIR}/schema/neutral-aggregate-v1.schema.json"
        TRACY_CANDIDATE_MANIFEST_SCHEMA_PATH="${CMAKE_CURRENT_LIST_DIR}/schema/candidate-manifest-v1.schema.json"
        TRACY_ANALYSIS_SCAN_API_SCHEMA_PATH="${CMAKE_CURRENT_LIST_DIR}/schema/analysis-scan-api-v1.schema.json"
    )
    tracy_query_enable_assertions(tracy-query-deterministic-scan-contract-tests)
    add_test(NAME tracy-query-deterministic-scan-contract COMMAND tracy-query-deterministic-scan-contract-tests)

    add_executable(tracy-analysis-profile-tests tests/AnalysisProfileTests.cpp)
    target_link_libraries(tracy-analysis-profile-tests PRIVATE TracyQueryProtocol)
    tracy_query_enable_assertions(tracy-analysis-profile-tests)
    add_test(NAME tracy-analysis-profile COMMAND tracy-analysis-profile-tests)

    add_executable(tracy-neutral-aggregate-store-tests tests/NeutralAggregateStoreTests.cpp)
    target_link_libraries(tracy-neutral-aggregate-store-tests PRIVATE TracyAnalysis nlohmann_json::nlohmann_json)
    tracy_query_enable_assertions(tracy-neutral-aggregate-store-tests)
    add_test(NAME tracy-neutral-aggregate-store COMMAND tracy-neutral-aggregate-store-tests)

    add_executable(tracy-analysis-cache-table-tests tests/AnalysisCacheTableTests.cpp)
    target_link_libraries(tracy-analysis-cache-table-tests PRIVATE TracyAnalysis)
    tracy_query_enable_assertions(tracy-analysis-cache-table-tests)
    add_test(NAME tracy-analysis-cache-table COMMAND tracy-analysis-cache-table-tests)
    add_executable(tracy-analysis-cache-query-tests tests/AnalysisCacheQueryTests.cpp)
    target_link_libraries(tracy-analysis-cache-query-tests PRIVATE TracyAnalysis nlohmann_json::nlohmann_json)
    tracy_query_enable_assertions(tracy-analysis-cache-query-tests)
    add_test(NAME tracy-analysis-cache-query COMMAND tracy-analysis-cache-query-tests)
    add_executable(tracy-analysis-workspace-budget-tests tests/AnalysisWorkspaceBudgetTests.cpp)
    target_link_libraries(tracy-analysis-workspace-budget-tests PRIVATE TracyAnalysis nlohmann_json::nlohmann_json)
    tracy_query_enable_assertions(tracy-analysis-workspace-budget-tests)
    add_test(NAME tracy-analysis-workspace-budget COMMAND tracy-analysis-workspace-budget-tests)
    add_executable(tracy-analysis-dictionary-tests tests/AnalysisDictionaryTests.cpp)
    target_include_directories(tracy-analysis-dictionary-tests PRIVATE ../analysis)
    tracy_query_enable_assertions(tracy-analysis-dictionary-tests)
    add_test(NAME tracy-analysis-dictionary COMMAND tracy-analysis-dictionary-tests)

    add_executable(tracy-neutral-statistics-cache-tests tests/NeutralStatisticsCacheTests.cpp)
    target_link_libraries(tracy-neutral-statistics-cache-tests PRIVATE TracyAnalysis nlohmann_json::nlohmann_json)
    tracy_query_enable_assertions(tracy-neutral-statistics-cache-tests)
    add_test(NAME tracy-neutral-statistics-cache COMMAND tracy-neutral-statistics-cache-tests)

    add_executable(tracy-policy-cache-input-tests tests/PolicyCacheInputTests.cpp)
    target_link_libraries(tracy-policy-cache-input-tests PRIVATE TracyAnalysis nlohmann_json::nlohmann_json)
    tracy_query_enable_assertions(tracy-policy-cache-input-tests)
    add_test(NAME tracy-policy-cache-input COMMAND tracy-policy-cache-input-tests)
    add_executable(tracy-candidate-policy-cache-tests tests/CandidatePolicyCacheTests.cpp)
    target_link_libraries(tracy-candidate-policy-cache-tests PRIVATE TracyAnalysis nlohmann_json::nlohmann_json)
    tracy_query_enable_assertions(tracy-candidate-policy-cache-tests)
    add_test(NAME tracy-candidate-policy-cache COMMAND tracy-candidate-policy-cache-tests)

    add_executable(tracy-bounded-scan-cursor-tests tests/BoundedScanCursorTests.cpp)
    target_link_libraries(tracy-bounded-scan-cursor-tests PRIVATE TracyAnalysis)
    tracy_query_enable_assertions(tracy-bounded-scan-cursor-tests)
    add_test(NAME tracy-bounded-scan-cursor COMMAND tracy-bounded-scan-cursor-tests)

    add_executable(tracy-frame-cpu-scanner-tests tests/FrameCpuScannerTests.cpp)
    target_link_libraries(tracy-frame-cpu-scanner-tests PRIVATE TracyAnalysis)
    tracy_query_enable_assertions(tracy-frame-cpu-scanner-tests)
    add_test(NAME tracy-frame-cpu-scanner COMMAND tracy-frame-cpu-scanner-tests)

    add_executable(tracy-gpu-job-managed-scanner-tests tests/GpuJobManagedScannerTests.cpp)
    target_link_libraries(tracy-gpu-job-managed-scanner-tests PRIVATE TracyAnalysis)
    target_include_directories(tracy-gpu-job-managed-scanner-tests PRIVATE ${TRACY_COMMON_DIR})
    tracy_query_enable_assertions(tracy-gpu-job-managed-scanner-tests)
    add_test(NAME tracy-gpu-job-managed-scanner COMMAND tracy-gpu-job-managed-scanner-tests)

    add_executable(tracy-memory-io-sampling-telemetry-scanner-tests tests/MemoryIoSamplingTelemetryScannerTests.cpp)
    target_link_libraries(tracy-memory-io-sampling-telemetry-scanner-tests PRIVATE TracyAnalysis)
    target_include_directories(tracy-memory-io-sampling-telemetry-scanner-tests PRIVATE ${TRACY_COMMON_DIR})
    tracy_query_enable_assertions(tracy-memory-io-sampling-telemetry-scanner-tests)
    add_test(NAME tracy-memory-io-sampling-telemetry-scanner COMMAND tracy-memory-io-sampling-telemetry-scanner-tests)

    add_executable(tracy-exact-statistics-tests tests/ExactStatisticsTests.cpp)
    target_link_libraries(tracy-exact-statistics-tests PRIVATE TracyAnalysis nlohmann_json::nlohmann_json)
    tracy_query_enable_assertions(tracy-exact-statistics-tests)
    add_test(NAME tracy-exact-statistics COMMAND tracy-exact-statistics-tests)

    add_executable(tracy-cache-publish-retry-tests tests/CachePublishRetryTests.cpp)
    target_link_libraries(tracy-cache-publish-retry-tests PRIVATE TracyAnalysis)
    tracy_query_enable_assertions(tracy-cache-publish-retry-tests)
    add_test(NAME tracy-cache-publish-retry COMMAND tracy-cache-publish-retry-tests)

    add_executable(tracy-frame-window-review-tests tests/FrameWindowReviewTests.cpp)
    target_link_libraries(tracy-frame-window-review-tests PRIVATE TracyAnalysis nlohmann_json::nlohmann_json)
    tracy_query_enable_assertions(tracy-frame-window-review-tests)
    add_test(NAME tracy-frame-window-review COMMAND tracy-frame-window-review-tests)

    add_executable(tracy-candidate-policy-tests tests/CandidatePolicyTests.cpp)
target_link_libraries(tracy-candidate-policy-tests PRIVATE TracyAnalysis nlohmann_json::nlohmann_json)
target_compile_definitions(tracy-candidate-policy-tests PRIVATE
    TRACY_CANDIDATE_MANIFEST_SCHEMA_PATH="${CMAKE_CURRENT_LIST_DIR}/schema/candidate-manifest-v1.schema.json")
    tracy_query_enable_assertions(tracy-candidate-policy-tests)
    add_test(NAME tracy-candidate-policy COMMAND tracy-candidate-policy-tests)

    add_executable(tracy-analysis-scan-manager-tests tests/AnalysisScanManagerTests.cpp)
    target_link_libraries(tracy-analysis-scan-manager-tests PRIVATE TracyQueryProtocol)
    tracy_query_enable_assertions(tracy-analysis-scan-manager-tests)
    add_test(NAME tracy-analysis-scan-manager COMMAND tracy-analysis-scan-manager-tests)
    # Same native-source assertions without linking the full QueryService
    # dispatcher; keeps default-producer red/green cycles independent of MCP.
    add_executable(tracy-default-analysis-cache-tests tests/AnalysisScanManagerTests.cpp src/TracyAnalysisScanManager.cpp src/TracyAnalysisScanCache.cpp)
    target_link_libraries(tracy-default-analysis-cache-tests PRIVATE TracyAnalysis nlohmann_json::nlohmann_json)
    target_include_directories(tracy-default-analysis-cache-tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
    target_compile_definitions(tracy-default-analysis-cache-tests PRIVATE TRACY_DEFAULT_SCAN_TEST_ONLY)
    tracy_query_enable_assertions(tracy-default-analysis-cache-tests)
    add_test(NAME tracy-default-analysis-cache COMMAND tracy-default-analysis-cache-tests)
    add_executable(tracy-analysis-cache-manager-tests tests/AnalysisScanManagerTests.cpp src/TracyAnalysisScanManager.cpp src/TracyAnalysisScanCache.cpp)
    target_link_libraries(tracy-analysis-cache-manager-tests PRIVATE TracyAnalysis nlohmann_json::nlohmann_json)
    target_include_directories(tracy-analysis-cache-manager-tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
    target_compile_definitions(tracy-analysis-cache-manager-tests PRIVATE TRACY_SCAN_MANAGER_TEST_ONLY)
    tracy_query_enable_assertions(tracy-analysis-cache-manager-tests)
    add_test(NAME tracy-analysis-cache-manager COMMAND tracy-analysis-cache-manager-tests)

    add_executable(tracy-gpu-analysis-tests tests/GpuAnalysisTests.cpp)
    target_link_libraries(tracy-gpu-analysis-tests PRIVATE TracyAnalysis)
    tracy_query_enable_assertions(tracy-gpu-analysis-tests)
    add_test(NAME tracy-gpu-analysis COMMAND tracy-gpu-analysis-tests)

    add_executable(tracy-query-mcp-transcript-tests tests/McpTranscriptTests.cpp)
    target_link_libraries(tracy-query-mcp-transcript-tests PRIVATE TracyQueryProtocol)
    tracy_query_enable_assertions(tracy-query-mcp-transcript-tests)
    add_test(NAME tracy-query-mcp-transcript COMMAND tracy-query-mcp-transcript-tests)

    # Explicit diagnostic CLI only; never opens a real Trace during CTest.
    add_executable(tracy-gpu-frame-scope-probe tests/GpuFrameScopeTraceProbe.cpp)
    target_link_libraries(tracy-gpu-frame-scope-probe PRIVATE TracyQueryProtocol)
    tracy_query_enable_assertions(tracy-gpu-frame-scope-probe)

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
            add_test(NAME tracy-query-worker-memory-frame COMMAND tracy-query
                --allow-root "${TRACY_ACCEPTANCE_TRACE_ROOT}"
                --trace "${TRACY_ACCEPTANCE_TRACE}"
                --request "${CMAKE_CURRENT_LIST_DIR}/tests/requests/memory-frame.json"
            )

            add_executable(tracy-query-mcp-worker-integration-tests tests/McpWorkerIntegrationTests.cpp)
            target_link_libraries(tracy-query-mcp-worker-integration-tests PRIVATE TracyQueryProtocol)
            tracy_query_enable_assertions(tracy-query-mcp-worker-integration-tests)
            target_compile_definitions(tracy-query-mcp-worker-integration-tests PRIVATE TRACY_ACCEPTANCE_TRACE="${TRACY_ACCEPTANCE_TRACE}")
            set(TRACY_ACCEPTANCE_COMPARE_TRACE "${TRACY_ACCEPTANCE_TRACE_ROOT}/Test-GPUMemory-Fresh2.tracy")
            if(EXISTS "${TRACY_ACCEPTANCE_COMPARE_TRACE}")
                target_compile_definitions(tracy-query-mcp-worker-integration-tests PRIVATE TRACY_ACCEPTANCE_COMPARE_TRACE="${TRACY_ACCEPTANCE_COMPARE_TRACE}")
            endif()
            set(TRACY_ACCEPTANCE_CONNECT_BASELINE "${TRACY_ACCEPTANCE_TRACE_ROOT}/Test-GPUMemory-Connect1.tracy")
            set(TRACY_ACCEPTANCE_CONNECT_CANDIDATE "${TRACY_ACCEPTANCE_TRACE_ROOT}/Test-GPUMemory-Connect2.tracy")
            if(EXISTS "${TRACY_ACCEPTANCE_CONNECT_BASELINE}" AND EXISTS "${TRACY_ACCEPTANCE_CONNECT_CANDIDATE}")
                target_compile_definitions(tracy-query-mcp-worker-integration-tests PRIVATE
                    TRACY_ACCEPTANCE_CONNECT_BASELINE="${TRACY_ACCEPTANCE_CONNECT_BASELINE}"
                    TRACY_ACCEPTANCE_CONNECT_CANDIDATE="${TRACY_ACCEPTANCE_CONNECT_CANDIDATE}")
            endif()
            set(TRACY_ACCEPTANCE_RECONNECT_BASELINE "${TRACY_ACCEPTANCE_TRACE_ROOT}/Test-GPU-Reconnect-1.tracy")
            set(TRACY_ACCEPTANCE_RECONNECT_CANDIDATE "${TRACY_ACCEPTANCE_TRACE_ROOT}/Test-GPU-Reconnect-2.tracy")
            if(EXISTS "${TRACY_ACCEPTANCE_RECONNECT_BASELINE}" AND EXISTS "${TRACY_ACCEPTANCE_RECONNECT_CANDIDATE}")
                target_compile_definitions(tracy-query-mcp-worker-integration-tests PRIVATE
                    TRACY_ACCEPTANCE_RECONNECT_BASELINE="${TRACY_ACCEPTANCE_RECONNECT_BASELINE}"
                    TRACY_ACCEPTANCE_RECONNECT_CANDIDATE="${TRACY_ACCEPTANCE_RECONNECT_CANDIDATE}")
            endif()
            set(TRACY_ACCEPTANCE_CORRUPT_TRACE "${TRACY_ACCEPTANCE_TRACE_ROOT}/Test-GPUMemory-Startup.tracy")
            if(EXISTS "${TRACY_ACCEPTANCE_CORRUPT_TRACE}")
                target_compile_definitions(tracy-query-mcp-worker-integration-tests PRIVATE TRACY_ACCEPTANCE_CORRUPT_TRACE="${TRACY_ACCEPTANCE_CORRUPT_TRACE}")
            endif()
            add_test(NAME tracy-query-mcp-worker-integration COMMAND tracy-query-mcp-worker-integration-tests)
        endif()
    endif()
endif()

if(WIN32)
    target_sources(tracy-query PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../cmake/WindowsUtf8.manifest)
endif()
