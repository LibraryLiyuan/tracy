#ifndef __TRACYTRACESESSIONFRAMEIMAGES_HPP__
#define __TRACYTRACESESSIONFRAMEIMAGES_HPP__

#include "TracyTraceSessionStore.hpp"
#include "TracyTraceSource.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tracy::analysis
{

inline constexpr uint32_t TraceSessionFrameImageIndexSchemaVersion = 1;

struct TraceSessionFrameImageStats
{
    uint64_t images = 0;
    uint64_t imageDataEvents = 0;
    uint64_t imageEvents = 0;
    uint64_t bc1Bytes = 0;
    uint64_t metadataFileBytes = 0;
    uint64_t dataFileBytes = 0;
};

struct TraceSessionFrameImageRecord
{
    uint32_t width = 0;
    uint32_t height = 0;
    bool flipped = false;
    uint32_t rawFrameIndex = 0;
    uint64_t dataOffset = 0;
    uint64_t dataBytes = 0;
};

class TraceSessionFrameImageReader
{
public:
    static std::shared_ptr<TraceSessionFrameImageReader> Open(
        const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest,
        std::string& error );

    const TraceSessionFrameImageStats& Stats() const { return m_stats; }
    const std::vector<TraceSessionFrameImageRecord>& Images() const { return m_images; }
    BinaryResourceChunkDto ReadBc1( size_t imageId, size_t offset, size_t maxBytes ) const;
    FrameImageDto Decode( size_t imageId, size_t maxBytes ) const;

private:
    std::filesystem::path m_dataPath;
    std::vector<TraceSessionFrameImageRecord> m_images;
    TraceSessionFrameImageStats m_stats;
};

std::filesystem::path TraceSessionFrameImageIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest );
bool BuildTraceSessionFrameImageDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionFrameImageStats& stats,
    std::string& error );
bool AuditTraceSessionFrameImageDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionFrameImageStats& stats,
    std::string& error );

}

#endif
