#ifndef __TRACYANALYSISIOPATH_HPP__
#define __TRACYANALYSISIOPATH_HPP__

#include <filesystem>
#include <string>

namespace tracy::analysis
{

// Normalize only native I/O paths for long Windows cache paths.
// Persisted manifests retain portable relative paths without the prefix.
inline std::filesystem::path AnalysisIoPath( const std::filesystem::path& input )
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
