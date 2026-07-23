set(TRACY_ANALYSIS_DIR ${CMAKE_CURRENT_LIST_DIR}/../analysis)

set(TRACY_ANALYSIS_SOURCES
    ${TRACY_ANALYSIS_DIR}/TracyAnalysis.cpp
    ${TRACY_ANALYSIS_DIR}/TracyHash.cpp
    ${TRACY_ANALYSIS_DIR}/TracyMemoryAnalysis.cpp
    ${TRACY_ANALYSIS_DIR}/TracyWorkerTraceSource.cpp
)

set(TRACY_ANALYSIS_HEADERS
    ${TRACY_ANALYSIS_DIR}/TracyAnalysis.hpp
    ${TRACY_ANALYSIS_DIR}/TracyHash.hpp
    ${TRACY_ANALYSIS_DIR}/TracyMemoryAnalysis.hpp
    ${TRACY_ANALYSIS_DIR}/TracyTraceSource.hpp
    ${TRACY_ANALYSIS_DIR}/TracyWorkerTraceSource.hpp
)

add_library(TracyAnalysis STATIC ${TRACY_ANALYSIS_SOURCES} ${TRACY_ANALYSIS_HEADERS})
target_include_directories(TracyAnalysis PUBLIC ${TRACY_ANALYSIS_DIR})
target_compile_features(TracyAnalysis PUBLIC cxx_std_20)
target_link_libraries(TracyAnalysis PRIVATE TracyServer)
