cmake_minimum_required(VERSION 3.16)

option(NO_ISA_EXTENSIONS "Disable ISA extensions" ON)
set(NO_STATISTICS OFF)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/version.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(
    tracy-sqliteexport
    LANGUAGES C CXX
    VERSION ${TRACY_VERSION_STRING}
)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/config.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/vendor.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/server.cmake)

add_executable(
    tracy-sqliteexport
    src/sqliteexport.cpp
)

target_link_libraries(
    tracy-sqliteexport
    PRIVATE
        TracyServer
        winsqlite3
        bcrypt
)

target_compile_definitions(
    tracy-sqliteexport
    PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
)

set_property(
    DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    PROPERTY VS_STARTUP_PROJECT tracy-sqliteexport
)
