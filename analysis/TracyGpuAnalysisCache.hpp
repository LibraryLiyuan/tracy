#ifndef __TRACYGPUANALYSISCACHE_HPP__
#define __TRACYGPUANALYSISCACHE_HPP__

#include "TracyGpuAnalysis.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace tracy::analysis
{

struct GpuAnalysisCacheIdentity
{
    std::string traceSha256;
    uint64_t traceSize = 0;
    std::string guiBuild;
};

bool SaveGpuAnalysisCache( const std::filesystem::path& path, const GpuAnalysisCacheIdentity& identity,
    const GpuAnalysisSnapshot& snapshot, std::string& error );
std::optional<GpuAnalysisSnapshot> LoadGpuAnalysisCache( const std::filesystem::path& path,
    const GpuAnalysisCacheIdentity& identity, std::string& error );

}

#endif
