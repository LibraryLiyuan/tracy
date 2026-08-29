#ifndef __TRACYGPUANALYSISPATH_HPP__
#define __TRACYGPUANALYSISPATH_HPP__

#include <filesystem>
#include <string>

namespace tracy::analysis
{

// Keep the public sidecar name adjacent to and derived from the trace name,
// while allowing its immutable shard and atomic-temporary suffixes to exceed
// the legacy Win32 MAX_PATH boundary.  This changes only the native I/O path;
// manifests continue to store portable relative paths without the prefix.
inline std::filesystem::path GpuAnalysisIoPath( const std::filesystem::path& input )
{
#ifdef _WIN32
    if( input.empty() ) return input;
    std::error_code ec;
    auto absolute = input.is_absolute() ? input : std::filesystem::absolute( input, ec );
    if( ec ) return input;
    // The extended-length namespace disables Win32's normal '/' to '\\'
    // translation, while manifest-relative paths intentionally use generic
    // separators. Normalize before adding the prefix.
    absolute.make_preferred();
    const auto native = absolute.native();
    if( native.rfind( L"\\\\?\\", 0 ) == 0 ) return absolute;
    if( native.rfind( L"\\\\", 0 ) == 0 ) return std::filesystem::path( L"\\\\?\\UNC\\" + native.substr( 2 ) );
    return std::filesystem::path( L"\\\\?\\" + native );
#else
    return input;
#endif
}

}

#endif
