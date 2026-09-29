include_guard(GLOBAL)
set(TRACY_STORAGE_DIR ${CMAKE_CURRENT_LIST_DIR}/../storage)
add_library(TracyStorageCore STATIC
    ${TRACY_STORAGE_DIR}/TracyConversionResources.cpp
    ${TRACY_STORAGE_DIR}/TracyResourceSetStore.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../analysis/TracyPagedRecordStore.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../analysis/TracyHash.cpp
)
target_include_directories(TracyStorageCore PUBLIC ${TRACY_STORAGE_DIR} ${CMAKE_CURRENT_LIST_DIR}/../analysis)
target_compile_features(TracyStorageCore PUBLIC cxx_std_20)
target_link_libraries(TracyStorageCore PUBLIC libzstd)
if(WIN32)
    target_link_libraries(TracyStorageCore PUBLIC Psapi Bcrypt)
endif()
