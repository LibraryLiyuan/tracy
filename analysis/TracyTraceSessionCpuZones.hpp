#ifndef __TRACYTRACESESSIONCPUZONES_HPP__
#define __TRACYTRACESESSIONCPUZONES_HPP__

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

inline constexpr uint32_t TraceSessionCpuZoneIndexSchemaVersion = 3;

struct TraceSessionCpuZoneStats
{
    uint64_t zones = 0;
    uint64_t completeZones = 0;
    uint64_t invalidTimingZones = 0;
    uint64_t sourceLocations = 0;
    uint64_t beginEvents = 0;
    uint64_t endEvents = 0;
    uint64_t zoneBlocks = 0;
    uint64_t childLinks = 0;
    uint64_t fileBytes = 0;
};

class TraceSessionCpuZoneReader
{
public:
    static std::shared_ptr<TraceSessionCpuZoneReader> Open(
        const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest,
        std::string& error );

    const TraceSessionCpuZoneStats& Stats() const { return m_stats; }
    std::vector<SourceLocationDto> Sources() const;
    std::vector<CallsiteDto> Callsites() const;
    std::vector<CpuZoneDto> Scan( const ScanRange& range ) const;
    std::vector<CpuZoneDto> ScanThread( std::string_view threadRef,
        const ScanRange& range ) const;
    std::optional<CpuZoneDto> Get( uint64_t id ) const;
    std::vector<CpuZoneDto> Children( uint64_t id, size_t offset, size_t limit ) const;

private:
    struct Impl;
    explicit TraceSessionCpuZoneReader( std::shared_ptr<Impl> impl );
    std::vector<CpuZoneDto> ScanImpl( const ScanRange& range,
        std::optional<uint64_t> thread ) const;
    std::shared_ptr<Impl> m_impl;
    TraceSessionCpuZoneStats m_stats;
};

std::filesystem::path TraceSessionCpuZoneIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest );
bool BuildTraceSessionCpuZoneDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionCpuZoneStats& stats, std::string& error );
bool AuditTraceSessionCpuZoneDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionCpuZoneStats& stats, std::string& error );
bool CleanupTraceSessionCpuZoneTemporaryFiles( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, std::string& error );

}

#endif
