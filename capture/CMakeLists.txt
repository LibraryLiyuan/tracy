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
target_link_libraries(tracy-stream-convert PRIVATE TracyServer TracyStreamCore)
if(JN_STREAM_CONVERT_DEV_TOOLS)
    target_compile_definitions(tracy-stream-convert PRIVATE JN_STREAM_CONVERT_DEV_TOOLS=1)
    add_executable(tracy-trace-compare src/trace-compare.cpp)
    target_link_libraries(tracy-trace-compare PRIVATE TracyServer TracyStreamCore)
endif()
if(WIN32)
    target_link_libraries(tracy-stream-convert PRIVATE Psapi Bcrypt)
endif()

add_executable(tracy-stream-inspect src/stream-inspect.cpp)
target_link_libraries(tracy-stream-inspect PRIVATE TracyServer TracyStreamCore)

install(TARGETS ${PROJECT_NAME} tracy-stream-convert DESTINATION ${CMAKE_INSTALL_BINDIR})

if(WIN32)
    target_sources(tracy-capture PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../cmake/WindowsUtf8.manifest)
    target_sources(tracy-stream-convert PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../cmake/WindowsUtf8.manifest)
endif()
