cmake_minimum_required(VERSION 3.16)

option(NO_ISA_EXTENSIONS "Disable ISA extensions (don't pass -march=native or -mcpu=native to the compiler)" OFF)
option(NO_STATISTICS "Disable calculation of statistics" ON)
option(JN_STREAM_CONVERT_DEV_TOOLS "Expose legacy/compare StreamConvert validation modes" OFF)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/version.cmake)

set(CMAKE_CXX_STANDARD 20)

project(
    tracy-capture
    LANGUAGES C CXX
    VERSION ${TRACY_VERSION_STRING}
)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/config.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/server_vendor.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/server.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/stream.cmake)

set(PROGRAM_FILES
    src/capture.cpp
)

add_executable(${PROJECT_NAME} ${PROGRAM_FILES} ${COMMON_FILES} ${SERVER_FILES})
target_link_libraries(${PROJECT_NAME} PRIVATE TracyServer TracyStreamCore TracyGetOpt)
set_property(DIRECTORY ${CMAKE_CURRENT_LIST_DIR} PROPERTY VS_STARTUP_PROJECT ${PROJECT_NAME})

add_executable(tracy-stream-convert src/stream-convert.cpp)
add_library(TracyOfflineConversion STATIC src/OfflineConversionStorage.cpp)
target_link_libraries(TracyOfflineConversion PUBLIC TracyStorageCore)
target_link_libraries(tracy-stream-convert PRIVATE TracyServer TracyStreamCore TracyOfflineConversion)
if(JN_STREAM_CONVERT_DEV_TOOLS)
    target_compile_definitions(tracy-stream-convert PRIVATE JN_STREAM_CONVERT_DEV_TOOLS=1)
    add_executable(tracy-trace-compare src/trace-compare.cpp)
    target_link_libraries(tracy-trace-compare PRIVATE TracyServer TracyStreamCore)
    add_executable(tracy-memory-attribution-smoke tests/MemoryAttributionSmoke.cpp)
    target_link_libraries(tracy-memory-attribution-smoke PRIVATE TracyServer)
endif()
if(WIN32)
    target_link_libraries(tracy-stream-convert PRIVATE Psapi Bcrypt)
endif()

add_executable(tracy-stream-inspect src/stream-inspect.cpp)
target_link_libraries(tracy-stream-inspect PRIVATE TracyServer TracyStreamCore)

install(TARGETS ${PROJECT_NAME} tracy-stream-convert DESTINATION ${CMAKE_INSTALL_BINDIR})

include(CTest)
if(BUILD_TESTING)
    add_executable(tracy-conversion-resources-tests tests/ConversionResourcesTests.cpp)
    target_link_libraries(tracy-conversion-resources-tests PRIVATE TracyStorageCore)
    add_test(NAME tracy-conversion-resources COMMAND tracy-conversion-resources-tests ${CMAKE_CURRENT_BINARY_DIR}/resources-test)
    add_executable(tracy-resource-set-store-tests tests/ResourceSetStoreTests.cpp)
    target_link_libraries(tracy-resource-set-store-tests PRIVATE TracyStorageCore)
    add_test(NAME tracy-resource-set-store COMMAND tracy-resource-set-store-tests ${CMAKE_CURRENT_BINARY_DIR}/resource-sets-test)
    add_executable(tracy-offline-storage-tests tests/OfflineStorageTests.cpp)
    target_link_libraries(tracy-offline-storage-tests PRIVATE TracyOfflineConversion)
    add_test(NAME tracy-offline-storage COMMAND tracy-offline-storage-tests ${CMAKE_CURRENT_BINARY_DIR}/offline-storage-test)
endif()
