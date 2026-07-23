set(TRACY_STREAM_DIR ${CMAKE_CURRENT_LIST_DIR}/../stream)

if(NOT TARGET TracyStreamCore)
    add_library(TracyStreamCore STATIC
        ${TRACY_STREAM_DIR}/src/TracyStreamJournal.cpp
        ${TRACY_STREAM_DIR}/src/TracyStreamJournal.hpp
        ${TRACY_STREAM_DIR}/src/TracyStreamProtocol.cpp
        ${TRACY_STREAM_DIR}/src/TracyStreamProtocol.hpp
        ${TRACY_STREAM_DIR}/src/TracyStreamStore.cpp
        ${TRACY_STREAM_DIR}/src/TracyStreamStore.hpp
        ${CMAKE_CURRENT_LIST_DIR}/../server/TracyProtocolObserver.hpp
    )
    target_include_directories(TracyStreamCore PUBLIC ${TRACY_STREAM_DIR}/src)
    target_compile_features(TracyStreamCore PUBLIC cxx_std_20)
    if(WIN32)
        target_compile_definitions(TracyStreamCore PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
endif()
