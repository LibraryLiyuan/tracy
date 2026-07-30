cmake_minimum_required(VERSION 3.20)

project(JNTracyClient LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "JNTracyClient reference build currently supports Windows x64 only.")
endif()

add_library(JNTracyClient SHARED
    src/JNTracyClient.cpp
    ../public/TracyClient.cpp
)
target_compile_features(JNTracyClient PRIVATE cxx_std_17)
target_compile_definitions(JNTracyClient PRIVATE
    JN_TRACY_EXPORTS
    TRACY_ENABLE
    TRACY_ON_DEMAND
    TRACY_DELAYED_INIT
    TRACY_MANUAL_LIFETIME
    TRACY_NO_CRASH_HANDLER
)
target_include_directories(JNTracyClient PUBLIC include PRIVATE ../public)
set_target_properties(JNTracyClient PROPERTIES
    OUTPUT_NAME JNTracyClient
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
if(MSVC)
    target_compile_options(JNTracyClient PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(JNTracyClient PRIVATE $<$<CONFIG:Release>:/DEBUG:FULL>)
endif()

add_executable(JNTracyHarness tests/JNTracyHarness.cpp)
target_compile_features(JNTracyHarness PRIVATE cxx_std_17)
target_link_libraries(JNTracyHarness PRIVATE JNTracyClient)
set_target_properties(JNTracyHarness PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

enable_testing()
add_test(NAME jn-tracy-lifecycle COMMAND JNTracyHarness --hold-ms 1)
