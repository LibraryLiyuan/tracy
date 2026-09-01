#ifndef __TRACYTRACESESSIONEXPORT_HPP__
#define __TRACYTRACESESSIONEXPORT_HPP__

#include "TracyTraceSessionStore.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace tracy::analysis
{

struct TraceSessionExportSelection
{
    std::optional<int64_t> timeBeginNs;
    std::optional<int64_t> timeEndNs;
    std::optional<uint64_t> frameSet;
    std::optional<uint64_t> frameBegin;
    std::optional<uint64_t> frameEnd;
};

struct TraceSessionExportRange
{
    int64_t beginNs = 0;
    int64_t endNs = 0;
    bool frameSelection = false;
    uint64_t frameSet = 0;
    uint64_t frameBegin = 0;
    uint64_t frameEnd = 0;
};

bool ResolveTraceSessionExportRange( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, const TraceSessionExportSelection& selection,
    TraceSessionExportRange& range, std::string& error );

}

#endif
