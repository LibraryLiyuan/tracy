#ifndef __TRACYTRACESESSIONCANONICAL_HPP__
#define __TRACYTRACESESSIONCANONICAL_HPP__

#include "TracyTraceSessionInventory.hpp"
#include "TracyTraceSessionStore.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace tracy::analysis
{

struct TraceSessionCanonicalOptions
{
    uint64_t targetShardBytes = 256ull * 1024 * 1024;
    uint64_t softShardBytes = 384ull * 1024 * 1024;
    uint64_t hardShardBytes = 512ull * 1024 * 1024;
    uint64_t minimumShardSpanNs = 5ull * 1000 * 1000 * 1000;
    uint64_t maximumShardSpanNs = 30ull * 1000 * 1000 * 1000;
};

bool BuildTraceSessionCanonical( const std::filesystem::path& sourcePath,
    const std::filesystem::path& sessionRoot, const std::string& generation,
    const TraceSessionInventory& inventory, const TraceSessionCanonicalOptions& options,
    TraceSessionManifest& manifest, std::string& error );

}

#endif
