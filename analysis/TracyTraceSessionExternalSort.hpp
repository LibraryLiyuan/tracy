#ifndef __TRACYTRACESESSIONEXTERNALSORT_HPP__
#define __TRACYTRACESESSIONEXTERNALSORT_HPP__

#include <cstdint>
#include <filesystem>
#include <string>

namespace tracy::analysis
{

struct TraceSessionUInt64Pair
{
    uint64_t key = 0;
    uint64_t value = 0;
};

static_assert( sizeof( TraceSessionUInt64Pair ) == 16 );

bool SortTraceSessionUInt64Pairs(
    const std::filesystem::path& source,
    const std::filesystem::path& output,
    const std::filesystem::path& temporaryRoot,
    const std::string& prefix,
    uint64_t expectedCount,
    uint64_t maximumBufferedPairs,
    std::string& error );

bool CleanupTraceSessionExternalSortFiles(
    const std::filesystem::path& temporaryRoot,
    const std::string& prefix,
    std::string& error );

}

#endif
