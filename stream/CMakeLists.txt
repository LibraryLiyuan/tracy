cmake_minimum_required(VERSION 3.16)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/version.cmake)

project(
    tracy-stream
    LANGUAGES CXX
    VERSION ${TRACY_VERSION_STRING}
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/stream.cmake)

add_executable(tracy-stream src/main.cpp)
target_link_libraries(tracy-stream PRIVATE TracyStreamCore)
target_compile_definitions(tracy-stream PRIVATE TRACY_STREAM_VERSION="${PROJECT_VERSION}")
set_property(DIRECTORY ${CMAKE_CURRENT_LIST_DIR} PROPERTY VS_STARTUP_PROJECT tracy-stream)

include(CTest)
if(BUILD_TESTING)
    add_executable(tracy-stream-journal-tests tests/StreamJournalTests.cpp)
    target_link_libraries(tracy-stream-journal-tests PRIVATE TracyStreamCore)
    if(MSVC)
        target_compile_options(tracy-stream-journal-tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(tracy-stream-journal-tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME tracy-stream-journal COMMAND tracy-stream-journal-tests)
    add_test(NAME tracy-stream-version COMMAND tracy-stream --version)

    add_executable(tracy-stream-test-producer
        tests/StreamTestProducer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../public/TracyClient.cpp
    )
    target_include_directories(tracy-stream-test-producer PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../public)
    target_compile_definitions(tracy-stream-test-producer PRIVATE
        TRACY_ENABLE
        TRACY_ONLY_LOCALHOST
        TRACY_NO_BROADCAST
        TRACY_NO_CODE_TRANSFER
        TRACY_NO_CONTEXT_SWITCH
        TRACY_NO_SAMPLING
        TRACY_NO_SYSTEM_TRACING
        TRACY_NO_CRASH_HANDLER
        TRACY_NO_FRAME_IMAGE
    )
    if(WIN32)
        target_link_libraries(tracy-stream-test-producer PRIVATE ws2_32 dbghelp)
    endif()
endif()

install(TARGETS tracy-stream DESTINATION bin)
