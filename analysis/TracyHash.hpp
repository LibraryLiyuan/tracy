#ifndef __TRACYHASH_HPP__
#define __TRACYHASH_HPP__

#include <filesystem>
#include <string>

namespace tracy::analysis
{

std::string Sha256File( const std::filesystem::path& path );

}

#endif
