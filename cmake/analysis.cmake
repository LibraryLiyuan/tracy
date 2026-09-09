set(TRACY_ANALYSIS_DIR ${CMAKE_CURRENT_LIST_DIR}/../analysis)

set(TRACY_ANALYSIS_SOURCES
    ${TRACY_ANALYSIS_DIR}/TracyAnalysisProcessMemory.cpp
    ${TRACY_ANALYSIS_DIR}/TracyAnalysisCacheTable.cpp
    ${TRACY_ANALYSIS_DIR}/TracyAnalysisCacheSort.cpp
    ${TRACY_ANALYSIS_DIR}/TracyAnalysisCacheQuery.cpp
    ${TRACY_ANALYSIS_DIR}/TracyNeutralStatisticsCache.cpp
    ${TRACY_ANALYSIS_DIR}/TracyAnalysisExternalSort.cpp
    ${TRACY_ANALYSIS_DIR}/TracyAnalysisWriterLease.cpp
    ${TRACY_ANALYSIS_DIR}/TracyAnalysisProfile.cpp
    ${TRACY_ANALYSIS_DIR}/TracyAnalysis.cpp
    ${TRACY_ANALYSIS_DIR}/TracyBoundedScanCursor.cpp
    ${TRACY_ANALYSIS_DIR}/TracyCandidatePolicy.cpp
    ${TRACY_ANALYSIS_DIR}/TracyCandidatePolicyCache.cpp
    ${TRACY_ANALYSIS_DIR}/TracyExactStatistics.cpp
    ${TRACY_ANALYSIS_DIR}/TracyFrameCpuScanner.cpp
    ${TRACY_ANALYSIS_DIR}/TracyGpuJobManagedScanner.cpp
    ${TRACY_ANALYSIS_DIR}/TracyMemoryIoSamplingTelemetryScanner.cpp
    ${TRACY_ANALYSIS_DIR}/TracyHash.cpp
    ${TRACY_ANALYSIS_DIR}/TracyGpuAnalysis.cpp
    ${TRACY_ANALYSIS_DIR}/TracyGpuAnalysisCache.cpp
    ${TRACY_ANALYSIS_DIR}/TracyMemoryAnalysis.cpp
    ${TRACY_ANALYSIS_DIR}/TracyNeutralAggregateStore.cpp
    ${TRACY_ANALYSIS_DIR}/TracyWorkerTraceSource.cpp
)

set(TRACY_ANALYSIS_HEADERS
    ${TRACY_ANALYSIS_DIR}/TracyAnalysisProcessMemory.hpp
    ${TRACY_ANALYSIS_DIR}/TracyAnalysisCacheTable.hpp
    ${TRACY_ANALYSIS_DIR}/TracyAnalysisCacheSort.hpp
    ${TRACY_ANALYSIS_DIR}/TracyAnalysisCacheQuery.hpp
    ${TRACY_ANALYSIS_DIR}/TracyAnalysisWorkspaceBudget.hpp
    ${TRACY_ANALYSIS_DIR}/TracyAnalysisDictionary.hpp
    ${TRACY_ANALYSIS_DIR}/TracyNeutralStatisticsCache.hpp
    ${TRACY_ANALYSIS_DIR}/TracyDeterministicScanTypes.hpp
    ${TRACY_ANALYSIS_DIR}/TracyAnalysis.hpp
    ${TRACY_ANALYSIS_DIR}/TracyBoundedScanCursor.hpp
    ${TRACY_ANALYSIS_DIR}/TracyCandidatePolicy.hpp
    ${TRACY_ANALYSIS_DIR}/TracyCandidatePolicyCache.hpp
    ${TRACY_ANALYSIS_DIR}/TracyExactStatistics.hpp
    ${TRACY_ANALYSIS_DIR}/TracyFrameCpuScanner.hpp
    ${TRACY_ANALYSIS_DIR}/TracyGpuJobManagedScanner.hpp
    ${TRACY_ANALYSIS_DIR}/TracyMemoryIoSamplingTelemetryScanner.hpp
    ${TRACY_ANALYSIS_DIR}/TracyHash.hpp
    ${TRACY_ANALYSIS_DIR}/TracyGpuAnalysis.hpp
    ${TRACY_ANALYSIS_DIR}/TracyGpuAnalysisCache.hpp
    ${TRACY_ANALYSIS_DIR}/TracyMemoryAnalysis.hpp
    ${TRACY_ANALYSIS_DIR}/TracyNeutralAggregateStore.hpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSource.hpp
    ${TRACY_ANALYSIS_DIR}/TracyWorkerTraceSource.hpp
)

add_library(TracyAnalysis STATIC ${TRACY_ANALYSIS_SOURCES} ${TRACY_ANALYSIS_HEADERS})
target_include_directories(TracyAnalysis PUBLIC ${TRACY_ANALYSIS_DIR})
target_compile_features(TracyAnalysis PUBLIC cxx_std_20)
target_link_libraries(TracyAnalysis PRIVATE TracyServer nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(TracyAnalysis PUBLIC Psapi Bcrypt)
endif()
