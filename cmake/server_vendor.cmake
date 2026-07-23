# Minimal dependency set for command-line programs that only link TracyServer.
# The full vendor.cmake also configures the profiler UI, importers, and query
# dependencies, which makes capture-only builds unnecessarily large.

include(FindPkgConfig)
include(${CMAKE_CURRENT_LIST_DIR}/CPM.cmake)

option(DOWNLOAD_CAPSTONE "Force download capstone" ON)

pkg_check_modules(CAPSTONE capstone)
if(CAPSTONE_FOUND AND NOT DOWNLOAD_CAPSTONE)
    message(STATUS "Capstone found: ${CAPSTONE}")
    add_library(TracyCapstone INTERFACE)
    target_include_directories(TracyCapstone INTERFACE ${CAPSTONE_INCLUDE_DIRS})
    target_link_libraries(TracyCapstone INTERFACE ${CAPSTONE_LINK_LIBRARIES})
else()
    CPMAddPackage(
        NAME capstone
        GITHUB_REPOSITORY capstone-engine/capstone
        GIT_TAG 6.0.0-Alpha5
        OPTIONS
            "CAPSTONE_X86_ATT_DISABLE ON"
            "CAPSTONE_ALPHA_SUPPORT OFF"
            "CAPSTONE_ARC_SUPPORT OFF"
            "CAPSTONE_HPPA_SUPPORT OFF"
            "CAPSTONE_LOONGARCH_SUPPORT OFF"
            "CAPSTONE_M680X_SUPPORT OFF"
            "CAPSTONE_M68K_SUPPORT OFF"
            "CAPSTONE_MIPS_SUPPORT OFF"
            "CAPSTONE_MOS65XX_SUPPORT OFF"
            "CAPSTONE_PPC_SUPPORT OFF"
            "CAPSTONE_SPARC_SUPPORT OFF"
            "CAPSTONE_SYSTEMZ_SUPPORT OFF"
            "CAPSTONE_XCORE_SUPPORT OFF"
            "CAPSTONE_TRICORE_SUPPORT OFF"
            "CAPSTONE_TMS320C64X_SUPPORT OFF"
            "CAPSTONE_EVM_SUPPORT OFF"
            "CAPSTONE_WASM_SUPPORT OFF"
            "CAPSTONE_BPF_SUPPORT OFF"
            "CAPSTONE_RISCV_SUPPORT OFF"
            "CAPSTONE_SH_SUPPORT OFF"
            "CAPSTONE_XTENSA_SUPPORT OFF"
            "CAPSTONE_BUILD_MACOS_THIN ON"
        EXCLUDE_FROM_ALL TRUE
    )
    add_library(TracyCapstone INTERFACE)
    target_include_directories(TracyCapstone INTERFACE ${capstone_SOURCE_DIR}/include/capstone)
    target_link_libraries(TracyCapstone INTERFACE capstone_static)
endif()

CPMAddPackage(
    NAME zstd
    GITHUB_REPOSITORY facebook/zstd
    GIT_TAG v1.5.7
    OPTIONS
        "ZSTD_BUILD_SHARED OFF"
    EXCLUDE_FROM_ALL TRUE
    SOURCE_SUBDIR build/cmake
)

if(CPM_PPQSort_SOURCE)
    # A caller-provided source tree may already contain the Tracy patch. Avoid
    # mutating external/read-only dependency caches.
    add_library(PPQSort INTERFACE)
    add_library(PPQSort::PPQSort ALIAS PPQSort)
    target_include_directories(PPQSort INTERFACE ${CPM_PPQSort_SOURCE}/include)
else()
    CPMAddPackage(
        NAME PPQSort
        GITHUB_REPOSITORY GabTux/PPQSort
        VERSION 1.0.6
        PATCHES
            "${CMAKE_CURRENT_LIST_DIR}/ppqsort-nodebug.patch"
        EXCLUDE_FROM_ALL TRUE
    )
endif()

set(GETOPT_DIR "${CMAKE_CURRENT_LIST_DIR}/../getopt")
set(GETOPT_SOURCES ${GETOPT_DIR}/getopt.c)
set(GETOPT_HEADERS ${GETOPT_DIR}/getopt.h)
add_library(TracyGetOpt STATIC EXCLUDE_FROM_ALL ${GETOPT_SOURCES} ${GETOPT_HEADERS})
target_include_directories(TracyGetOpt PUBLIC ${GETOPT_DIR})
