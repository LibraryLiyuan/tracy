#ifndef __TRACYTRACESESSIONGPUZONES_HPP__
#define __TRACYTRACESESSIONGPUZONES_HPP__

#include "TracyTraceSessionStore.hpp"
#include "TracyTraceSource.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tracy::analysis
{

inline constexpr uint32_t TraceSessionGpuZoneIndexSchemaVersion = 2;

struct TraceSessionGpuZoneStats
{
    uint64_t contexts = 0;
    uint64_t zones = 0;
    uint64_t zoneBlocks = 0;
    uint64_t completeZones = 0;
    uint64_t sourceLocations = 0;
    uint64_t beginEvents = 0;
    uint64_t endEvents = 0;
    uint64_t gpuTimeEvents = 0;
    uint64_t calibrationEvents = 0;
    uint64_t syncEvents = 0;
    uint64_t fileBytes = 0;
};

class TraceSessionGpuZoneReader
{
public:
    static std::shared_ptr<TraceSessionGpuZoneReader> Open(
        const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest,
        std::string& error );

    const TraceSessionGpuZoneStats& Stats() const { return m_stats; }
    const std::vector<GpuContextDto>& Contexts() const { return m_contexts; }
    std::vector<GpuZoneDto> Scan( const ScanRange& range ) const;
    std::vector<GpuZoneDto> ScanContext( std::string_view contextRef,
        const ScanRange& range ) const;
    std::optional<GpuZoneDto> Get( uint64_t id ) const;
    std::vector<GpuZoneDto> Children( uint64_t id, size_t offset, size_t limit ) const;

private:
    struct Impl;
    explicit TraceSessionGpuZoneReader( std::shared_ptr<Impl> impl );
    std::vector<GpuZoneDto> ScanImpl( const ScanRange& range,
        std::optional<uint32_t> context ) const;
    std::shared_ptr<Impl> m_impl;
    TraceSessionGpuZoneStats m_stats;
    std::vector<GpuContextDto> m_contexts;
};

std::filesystem::path TraceSessionGpuZoneIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest );
bool BuildTraceSessionGpuZoneDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionGpuZoneStats& stats, std::string& error );
bool CleanupTraceSessionGpuZoneTemporaryFiles( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, std::string& error );
bool AuditTraceSessionGpuZoneDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionGpuZoneStats& stats, std::string& error );

}

#endif
