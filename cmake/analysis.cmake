set(TRACY_ANALYSIS_DIR ${CMAKE_CURRENT_LIST_DIR}/../analysis)

set(TRACY_ANALYSIS_SOURCES
    ${TRACY_ANALYSIS_DIR}/TracyAnalysis.cpp
    ${TRACY_ANALYSIS_DIR}/TracyHash.cpp
    ${TRACY_ANALYSIS_DIR}/TracyGpuAnalysis.cpp
    ${TRACY_ANALYSIS_DIR}/TracyGpuAnalysisCache.cpp
    ${TRACY_ANALYSIS_DIR}/TracyGpuAnalysisSidecar.cpp
    ${TRACY_ANALYSIS_DIR}/TracyGpuAnalysisStore.cpp
    ${TRACY_ANALYSIS_DIR}/TracyGpuAnalysisTraceSource.cpp
    ${TRACY_ANALYSIS_DIR}/TracyMemoryAnalysis.cpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionCanonical.cpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionDerived.cpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionFrames.cpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionFrameImages.cpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionJobs.cpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionCpuZones.cpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionGpuZones.cpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionMemory.cpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionSampling.cpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionScheduling.cpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionGpuCanonical.cpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionInventory.cpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionProtocolInventory.cpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionStore.cpp
    ${TRACY_ANALYSIS_DIR}/TracyWorkerTraceSource.cpp
)

set(TRACY_ANALYSIS_HEADERS
    ${TRACY_ANALYSIS_DIR}/TracyAnalysis.hpp
    ${TRACY_ANALYSIS_DIR}/TracyHash.hpp
    ${TRACY_ANALYSIS_DIR}/TracyGpuAnalysis.hpp
    ${TRACY_ANALYSIS_DIR}/TracyGpuAnalysisCache.hpp
    ${TRACY_ANALYSIS_DIR}/TracyGpuAnalysisSidecar.hpp
    ${TRACY_ANALYSIS_DIR}/TracyGpuAnalysisStore.hpp
    ${TRACY_ANALYSIS_DIR}/TracyGpuAnalysisTraceSource.hpp
    ${TRACY_ANALYSIS_DIR}/TracyMemoryAnalysis.hpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSource.hpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionCanonical.hpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionDerived.hpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionFrames.hpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionFrameImages.hpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionJobs.hpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionCpuZones.hpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionGpuZones.hpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionMemory.hpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionSampling.hpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionScheduling.hpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionGpuCanonical.hpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionInventory.hpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionProtocolInventory.hpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSessionStore.hpp
    ${TRACY_ANALYSIS_DIR}/TracyWorkerTraceSource.hpp
)

add_library(TracyAnalysis STATIC ${TRACY_ANALYSIS_SOURCES} ${TRACY_ANALYSIS_HEADERS})
target_include_directories(TracyAnalysis PUBLIC ${TRACY_ANALYSIS_DIR})
target_compile_features(TracyAnalysis PUBLIC cxx_std_20)
target_link_libraries(TracyAnalysis PRIVATE TracyServer TracyStreamCore)
