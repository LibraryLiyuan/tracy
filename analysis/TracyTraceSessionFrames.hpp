#ifndef __TRACYTRACESESSIONFRAMES_HPP__
#define __TRACYTRACESESSIONFRAMES_HPP__

#include "TracyTraceSessionStore.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tracy::analysis
{

inline constexpr uint32_t TraceSessionFrameIndexSchemaVersion = 1;

struct TraceSessionFrameRecord
{
    int64_t beginNs = 0;
    int64_t endNs = 0;
    bool complete = false;
};

struct TraceSessionFrameSetRecord
{
    std::string name;
    bool continuous = false;
    std::vector<TraceSessionFrameRecord> frames;
};

struct TraceSessionFrameStats
{
    uint64_t frameSets = 0;
    uint64_t frames = 0;
    uint64_t completeFrames = 0;
    uint64_t fileBytes = 0;
};

class TraceSessionFrameReader
{
public:
    static std::shared_ptr<TraceSessionFrameReader> Open(
        const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest,
        std::string& error );

    const std::vector<TraceSessionFrameSetRecord>& Sets() const { return m_sets; }
    const TraceSessionFrameStats& Stats() const { return m_stats; }

private:
    std::vector<TraceSessionFrameSetRecord> m_sets;
    TraceSessionFrameStats m_stats;
};

std::filesystem::path TraceSessionFrameIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest );
bool BuildTraceSessionFrameDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, bool semanticTimePresent,
    int64_t lastSemanticTimeRaw, TraceSessionFrameStats& stats, std::string& error );

}

#endif
