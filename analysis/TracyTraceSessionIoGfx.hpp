#ifndef __TRACYTRACESESSIONIOGFX_HPP__
#define __TRACYTRACESESSIONIOGFX_HPP__

#include "TracyTraceSessionStore.hpp"
#include "TracyTraceSource.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tracy::analysis
{

inline constexpr uint32_t TraceSessionIoGfxIndexSchemaVersion = 4;

struct TraceSessionIoGfxStats
{
    uint64_t ioRequests = 0;
    uint64_t ioConfigs = 0;
    uint64_t ioStages = 0;
    uint64_t ioRequestIds = 0;
    uint64_t gfxDispatches = 0;
    uint64_t gfxEntities = 0;
    uint64_t gfxParentLinks = 0;
    uint64_t gfxLinks = 0;
    uint64_t correlatedFrames = 0;
    uint64_t fileBytes = 0;
};

class TraceSessionIoGfxReader
{
public:
    static std::shared_ptr<TraceSessionIoGfxReader> Open(
        const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest,
        std::string& error );

    const TraceSessionIoGfxStats& Stats() const { return m_stats; }
    std::vector<IoRequestDto> IoRequests() const;
    std::optional<IoRequestDto> IoRequest( uint64_t requestId ) const;
    std::vector<GfxDispatchDto> GfxDispatches() const;
    std::vector<GfxDispatchDto> GfxDispatchesForFrame(
        uint64_t frameId, size_t offset, size_t limit ) const;
    std::vector<GfxEntityDto> GfxEntities() const;
    std::vector<GfxLinkDto> GfxLinks() const;
    GfxEvidenceSlice EvidenceGfx( uint64_t frameId,
        const std::vector<uint64_t>& seedIds ) const;
    std::vector<CorrelatedFrameEventDto> CorrelatedFrames() const;
    std::vector<CorrelatedFrameEventDto> CorrelatedFramesForFrame(
        uint64_t frameId, size_t offset, size_t limit ) const;

private:
    std::filesystem::path m_path;
    std::string m_fingerprint;
    uint64_t m_ioRequestOffset = 0;
    uint64_t m_ioConfigOffset = 0;
    uint64_t m_ioStageOffset = 0;
    uint64_t m_ioRequestIdPostingOffset = 0;
    uint64_t m_ioRequestIdPostingCount = 0;
    uint64_t m_gfxDispatchOffset = 0;
    uint64_t m_gfxEntityOffset = 0;
    uint64_t m_gfxLinkOffset = 0;
    uint64_t m_frameOffset = 0;
    uint64_t m_dispatchFramePostingOffset = 0;
    uint64_t m_correlatedFramePostingOffset = 0;
    uint64_t m_gfxEntityIdPostingOffset = 0;
    uint64_t m_gfxParentPostingOffset = 0;
    uint64_t m_gfxLinkSourcePostingOffset = 0;
    uint64_t m_gfxLinkTargetPostingOffset = 0;
    TraceSessionIoGfxStats m_stats;
};

std::filesystem::path TraceSessionIoGfxIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest );
bool BuildTraceSessionIoGfxDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionIoGfxStats& stats, std::string& error );
bool AuditTraceSessionIoGfxDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionIoGfxStats& stats, std::string& error );

}

#endif
